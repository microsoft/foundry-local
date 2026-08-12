// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// End-to-end coverage for creating missing local-model metadata during registration and then running inference.

#include "model_fixture.h"

#include "internal_api/test_model_cache.h"
#include "utils/temp_path.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

namespace {

constexpr const char* kByomSourceModelEnvironmentVariable = "FOUNDRY_LOCAL_BYOM_TEST_MODEL_PATH";
constexpr const char* kByomChatModelAlias = "qwen2.5-coder-0.5b-instruct-generic-cpu-3";

std::optional<fs::path> FindGenAiModelDirectory(const fs::path& model_path) {
  if (fs::exists(model_path / "genai_config.json")) {
    return fs::canonical(model_path);
  }

  if (!fs::is_directory(model_path)) {
    return std::nullopt;
  }

  for (const auto& entry : fs::directory_iterator(model_path)) {
    if (entry.is_directory() && fs::exists(entry.path() / "genai_config.json")) {
      return fs::canonical(entry.path());
    }
  }

  // Shared test data uses <root>/Microsoft/<alias>/<variant>. Also accept <root>/<alias> as the override so the
  // documented path remains useful even when that directory is an empty catalog placeholder.
  const auto publisher_model_path = model_path.parent_path() / "Microsoft" / model_path.filename();
  if (publisher_model_path != model_path && fs::is_directory(publisher_model_path)) {
    return FindGenAiModelDirectory(publisher_model_path);
  }

  return std::nullopt;
}

std::optional<fs::path> GetByomSourceModelPath() {
  const auto override_path = fl::test::SafeGetEnv(kByomSourceModelEnvironmentVariable);
  if (!override_path.empty()) {
    return FindGenAiModelDirectory(override_path);
  }

  try {
    return fl::test::GetTestModelPath(kByomChatModelAlias);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

void StageModelWithoutMetadata(const fs::path& source, const fs::path& destination) {
  for (const auto& entry : fs::recursive_directory_iterator(source)) {
    const auto relative_path = fs::relative(entry.path(), source);
    if (relative_path.filename() == "inference_model.json" || relative_path.filename() == "model_metadata.yml") {
      continue;
    }

    const auto destination_path = destination / relative_path;
    if (entry.is_directory()) {
      fs::create_directories(destination_path);
      continue;
    }
    if (!entry.is_regular_file()) {
      continue;
    }

    fs::create_directories(destination_path.parent_path());
    std::error_code hard_link_error;
    fs::create_hard_link(entry.path(), destination_path, hard_link_error);
    if (hard_link_error) {
      fs::copy_file(entry.path(), destination_path, fs::copy_options::overwrite_existing);
    }
  }
}

class LocalRegistrationGuard {
 public:
  LocalRegistrationGuard(foundry_local::ICatalog& catalog, std::unique_ptr<foundry_local::IModel> model,
                         std::string alias)
      : catalog_(catalog), model_(std::move(model)), alias_(std::move(alias)) {}

  ~LocalRegistrationGuard() {
    try {
      if (model_ && model_->IsLoaded()) {
        model_->Unload();
      }
    } catch (...) {
    }

    model_.reset();
    try {
      catalog_.UnregisterModel(alias_);
    } catch (...) {
    }
  }

  foundry_local::IModel& model() { return *model_; }

 private:
  foundry_local::ICatalog& catalog_;
  std::unique_ptr<foundry_local::IModel> model_;
  std::string alias_;
};

}  // namespace

TEST(ByomE2eTest, RegisterModelCreatesMissingMetadataAndRunsChatInference) {
  using namespace foundry_local;

  const auto source_model_path = GetByomSourceModelPath();
  if (!source_model_path) {
    GTEST_SKIP() << "BYOM source model not found. Set " << kByomSourceModelEnvironmentVariable
                 << " or stage " << kByomChatModelAlias << " under FOUNDRY_TEST_DATA_DIR/Microsoft.";
  }

  auto temp_root = fl::test::TempPath::CreateTempDir("fl_byom_e2e_");
  const auto staged_model_path = temp_root.path() / "model";
  fs::create_directories(staged_model_path);
  StageModelWithoutMetadata(*source_model_path, staged_model_path);

  ASSERT_TRUE(fs::exists(staged_model_path / "genai_config.json"));
  ASSERT_FALSE(fs::exists(staged_model_path / "inference_model.json"));
  ASSERT_FALSE(fs::exists(staged_model_path / "model_metadata.yml"));

  auto& local_catalog = SharedTestEnv::Get().manager()->GetCatalog(CatalogType::Local);
  const auto registration_alias = temp_root.path().filename().string();
  ModelInfo registration;
  registration.SetStringProperty(FOUNDRY_LOCAL_REG_MODEL_PATH, staged_model_path.string().c_str());
  registration.SetStringProperty(FOUNDRY_LOCAL_REG_ALIAS, registration_alias.c_str());

  LocalRegistrationGuard registered(local_catalog, local_catalog.RegisterModel(registration), registration_alias);
  auto& model = registered.model();

  EXPECT_EQ(model.GetInfo().Alias(), registration_alias);
  EXPECT_EQ(model.GetInfo().Task(), "chat-completion");
  EXPECT_TRUE(model.IsCached());
  EXPECT_FALSE(model.IsLoaded());
  EXPECT_TRUE(fs::exists(staged_model_path / "model_metadata.yml"));
  EXPECT_FALSE(fs::exists(staged_model_path / "inference_model.json"));

  model.Load();
  ASSERT_TRUE(model.IsLoaded());

  ChatSession session(model);
  Request request{
      SystemMessage("You are a concise math assistant."),
      UserMessage("What is 2+2? Answer with only the number."),
  };
  RequestOptions options;
  options.search.temperature = 0.0f;
  options.search.max_output_tokens = 16;
  request.SetOptions(options);

  Response response = session.ProcessRequest(request);

  EXPECT_EQ(response.GetFinishReason(), FOUNDRY_LOCAL_FINISH_STOP);
  const auto response_text = CollectResponseText(response);
  EXPECT_FALSE(response_text.empty());
  EXPECT_NE(response_text.find('4'), std::string::npos) << "Unexpected response: " << response_text;
}