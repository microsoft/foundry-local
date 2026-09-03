// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// End-to-end coverage for registering existing local model assets and then running inference.

#include <foundry_local/foundry_local_cpp.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "internal_api/test_model_cache.h"
#include "utils/temp_path.h"

#include <filesystem>
#include <fstream>
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

void StageModelAssets(const fs::path& source, const fs::path& destination) {
  for (const auto& entry : fs::recursive_directory_iterator(source)) {
    const auto relative_path = fs::relative(entry.path(), source);
    if (relative_path.filename() == "inference_model.json") {
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

std::string CollectResponseText(const foundry_local::Response& response) {
  std::string text;
  for (const auto& item : response.GetItems()) {
    if (item.GetType() == FOUNDRY_LOCAL_ITEM_TEXT) {
      text += item.GetText().text;
    } else if (item.GetType() == FOUNDRY_LOCAL_ITEM_MESSAGE) {
      for (const auto& part : item.GetMessage().parts) {
        if (part.GetType() == FOUNDRY_LOCAL_ITEM_TEXT) {
          text += part.GetText().text;
        }
      }
    }
  }

  return text;
}

class LocalRegistrationGuard {
 public:
  LocalRegistrationGuard(foundry_local::ICatalog& catalog,
                         std::unique_ptr<foundry_local::IModel> model,
                         std::string model_id)
      : catalog_(catalog), model_(std::move(model)), model_id_(std::move(model_id)) {}

  ~LocalRegistrationGuard() {
    try {
      if (model_ && model_->IsLoaded()) {
        model_->Unload();
      }
    } catch (const std::exception& ex) {
      ADD_FAILURE() << "Failed to unload local registration " << model_id_ << ": " << ex.what();
    } catch (...) {
      ADD_FAILURE() << "Failed to unload local registration " << model_id_ << ": unknown error";
    }

    model_.reset();
    try {
      catalog_.UnregisterModel(model_id_);
    } catch (const std::exception& ex) {
      ADD_FAILURE() << "Failed to unregister local model " << model_id_ << ": " << ex.what();
    } catch (...) {
      ADD_FAILURE() << "Failed to unregister local model " << model_id_ << ": unknown error";
    }
  }

  foundry_local::IModel& model() { return *model_; }

 private:
  foundry_local::ICatalog& catalog_;
  std::unique_ptr<foundry_local::IModel> model_;
  std::string model_id_;
};

}  // namespace

TEST(ByomE2eTest, RegisterModelPreservesAssetsAndRunsChatInference) {
  using namespace foundry_local;

  const auto source_model_path = GetByomSourceModelPath();
  if (!source_model_path) {
    GTEST_SKIP() << "BYOM source model not found. Set " << kByomSourceModelEnvironmentVariable
                 << " or stage " << kByomChatModelAlias << " under FOUNDRY_TEST_DATA_DIR/Microsoft.";
  }

  auto temp_root = fl::test::TempPath::CreateTempDir("fl_byom_e2e_");
  const auto staged_model_path = temp_root.path() / "model";
  const auto app_data_path = temp_root.path() / "appdata";
  const auto model_cache_path = temp_root.path() / "cache" / "models";
  fs::create_directories(staged_model_path);
  StageModelAssets(*source_model_path, staged_model_path);

  ASSERT_TRUE(fs::exists(staged_model_path / "genai_config.json"));
  ASSERT_FALSE(fs::exists(staged_model_path / "inference_model.json"));

  const auto registration_alias = temp_root.path().filename().string();
  const auto registration_id = registration_alias + ":1";
  const auto sibling_id = registration_alias + ":2";
  const auto index_path = model_cache_path / "foundry.local.modelinfo.json";

  {
    Configuration config("foundry_local_byom_inference_test");
    config.SetAppDataDir(app_data_path.string()).SetModelCacheDir(model_cache_path.string());
    Manager manager(std::move(config));
    auto& local_catalog = manager.GetCatalog(FOUNDRY_LOCAL_CATALOG_LOCAL);

    ModelInfo sibling_metadata;
    sibling_metadata.SetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "chat-completion");
    LocalRegistrationGuard registered_sibling(
        local_catalog, local_catalog.RegisterModel(staged_model_path.string(), sibling_id, sibling_metadata),
        sibling_id);

    ModelInfo metadata;
    metadata.SetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "chat-completion");
    LocalRegistrationGuard registered(
        local_catalog, local_catalog.RegisterModel(staged_model_path.string(), registration_id, metadata),
        registration_id);
    auto& model = registered.model();

    EXPECT_EQ(model.GetInfo().Id(), registration_id);
    EXPECT_EQ(model.GetInfo().Alias(), registration_alias);
    EXPECT_EQ(model.GetInfo().Version(), 1);
    EXPECT_EQ(model.GetInfo().Task(), "chat-completion");
    EXPECT_TRUE(model.IsCached());
    EXPECT_FALSE(model.IsLoaded());
    EXPECT_TRUE(fs::is_regular_file(staged_model_path / "genai_config.json"));
    EXPECT_FALSE(fs::exists(staged_model_path / "inference_model.json"));

    model.Load();
    ASSERT_TRUE(model.IsLoaded());
    EXPECT_THROW(local_catalog.UnregisterModel(registration_alias), Error);

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

  nlohmann::json index;
  std::ifstream(index_path) >> index;
  ASSERT_TRUE(index.contains("models"));
  ASSERT_TRUE(index["models"].is_array());
  EXPECT_TRUE(index["models"].empty());
}

TEST(ByomE2eTest, RegistrationsPersistAcrossManagerRecreation) {
  using namespace foundry_local;

  const auto source_model_path = GetByomSourceModelPath();
  if (!source_model_path) {
    GTEST_SKIP() << "BYOM source model not found. Set " << kByomSourceModelEnvironmentVariable
                 << " or stage " << kByomChatModelAlias << " under FOUNDRY_TEST_DATA_DIR/Microsoft.";
  }

  auto temp_root = fl::test::TempPath::CreateTempDir("fl_byom_persistence_");
  const auto staged_model_path = temp_root.path() / "model";
  const auto app_data_path = temp_root.path() / "appdata";
  const auto model_cache_path = temp_root.path() / "cache" / "models";
  fs::create_directories(staged_model_path);
  StageModelAssets(*source_model_path, staged_model_path);

  const auto registration_alias = temp_root.path().filename().string();
  const auto first_id = registration_alias + ":1";
  const auto second_id = registration_alias + ":2";
  constexpr const char* kPersistenceMarker = "persistence_marker";

  const auto make_config = [&app_data_path, &model_cache_path]() {
    Configuration config("foundry_local_byom_persistence_test");
    config.SetAppDataDir(app_data_path.string())
        .SetModelCacheDir(model_cache_path.string())
        .SetExternalServiceUrl("http://127.0.0.1:1");
    return config;
  };

  {
    Manager manager(make_config());
    auto& local_catalog = manager.GetCatalog(FOUNDRY_LOCAL_CATALOG_LOCAL);
    ModelInfo first_metadata;
    first_metadata.SetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "chat-completion");
    first_metadata.SetStringProperty(kPersistenceMarker, "first");
    ModelInfo second_metadata;
    second_metadata.SetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "chat-completion");
    second_metadata.SetStringProperty(kPersistenceMarker, "second");

    auto first = local_catalog.RegisterModel(staged_model_path.string(), first_id, first_metadata);
    auto second = local_catalog.RegisterModel(staged_model_path.string(), second_id, second_metadata);

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_TRUE(first->IsCached());
    EXPECT_TRUE(second->IsCached());
  }

  const auto index_path = model_cache_path / "foundry.local.modelinfo.json";
  ASSERT_TRUE(fs::is_regular_file(index_path));
  nlohmann::json index;
  std::ifstream(index_path) >> index;
  ASSERT_TRUE(index.contains("models"));
  ASSERT_TRUE(index["models"].is_array());
  ASSERT_EQ(index["models"].size(), 2u);
  for (const auto& registration : index["models"]) {
    EXPECT_EQ(fs::path(registration.at("model_path").get<std::string>()), fs::absolute(staged_model_path));
  }

  {
    Manager manager(make_config());
    auto& local_catalog = manager.GetCatalog(FOUNDRY_LOCAL_CATALOG_LOCAL);
    auto first = local_catalog.GetModelVariant(first_id);
    auto second = local_catalog.GetModelVariant(second_id);

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    const auto first_info = first->GetInfo();
    const auto second_info = second->GetInfo();
    EXPECT_EQ(first_info.Id(), first_id);
    EXPECT_EQ(second_info.Id(), second_id);
    const auto first_marker = first_info.GetStringProperty(kPersistenceMarker);
    const auto second_marker = second_info.GetStringProperty(kPersistenceMarker);
    ASSERT_TRUE(first_marker.has_value());
    ASSERT_TRUE(second_marker.has_value());
    EXPECT_EQ(*first_marker, "first");
    EXPECT_EQ(*second_marker, "second");
    EXPECT_TRUE(fs::equivalent(fs::path(std::string(first->GetPath())), staged_model_path));
    EXPECT_TRUE(fs::equivalent(fs::path(std::string(second->GetPath())), staged_model_path));

    first.reset();
    second.reset();
    local_catalog.UnregisterModel(first_id);
    local_catalog.UnregisterModel(second_id);
    EXPECT_EQ(local_catalog.GetModelVariant(first_id), nullptr);
    EXPECT_EQ(local_catalog.GetModelVariant(second_id), nullptr);
    EXPECT_TRUE(fs::is_regular_file(staged_model_path / "genai_config.json"));
  }

  std::ifstream cleaned_index_stream(index_path);
  ASSERT_TRUE(cleaned_index_stream);
  cleaned_index_stream >> index;
  ASSERT_TRUE(index.contains("models"));
  ASSERT_TRUE(index["models"].is_array());
  EXPECT_TRUE(index["models"].empty());
}
