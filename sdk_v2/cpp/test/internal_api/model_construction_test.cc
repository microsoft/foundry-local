// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "exception.h"
#include "internal_api/test_helpers.h"
#include "model.h"
#include "model_info.h"
#include "util/scope_guard.h"

#include <gtest/gtest.h>

#include <string>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <type_traits>
#include <utility>

using namespace fl;

static_assert(!std::is_default_constructible_v<Model>);
static_assert(std::is_nothrow_move_constructible_v<Model>);
static_assert(std::is_nothrow_move_assignable_v<Model>);

namespace {

Model MakeLeaf(std::string model_id = "test-model", std::string task = "chat-completion") {
  static fl::test::FakeServiceBindings svc;
  ModelInfo info;
  info.model_id = std::move(model_id);
  info.name = "test";
  info.version = 1;
  info.alias = "test-alias";
  info.task = std::move(task);
  return Model::FromModelInfo(std::move(info), "", svc.download_manager, svc.model_load_manager);
}

}  // namespace

TEST(ModelConstructionTest, LeafHasMetadataImmediately) {
  auto model = MakeLeaf();

  EXPECT_EQ(model.Info().model_id, "test-model");
  EXPECT_EQ(model.Info().alias, "test-alias");
  EXPECT_EQ(model.Info().task, "chat-completion");
}

TEST(ModelConstructionTest, LocalRegistrationHasMetadataImmediately) {
  static fl::test::FakeServiceBindings svc;
  ModelInfo info;
  info.model_id = "local-model:1";
  info.name = "local-model";
  info.version = 1;
  info.alias = "local-model";
  info.task = "chat-completion";

  auto model = Model::FromLocalRegistration(std::move(info), "model-path", svc.download_manager,
                                            svc.model_load_manager);

  EXPECT_EQ(model.Info().model_id, "local-model:1");
  EXPECT_EQ(model.Info().task, "chat-completion");
  EXPECT_EQ(model.Id(), "local-model:1");
}

TEST(ModelConstructionTest, ContainerHasSelectedMetadataImmediately) {
  auto container = Model::MakeContainer(MakeLeaf());

  ASSERT_TRUE(container.IsContainer());
  EXPECT_EQ(container.Info().model_id, "test-model");
  EXPECT_EQ(container.Info().task, "chat-completion");
}

TEST(ModelConstructionTest, MoveConstructionPreservesContainerMetadata) {
  auto source = Model::MakeContainer(MakeLeaf());

  Model moved(std::move(source));

  ASSERT_TRUE(moved.IsContainer());
  EXPECT_EQ(moved.Info().model_id, "test-model");
  EXPECT_EQ(moved.Info().task, "chat-completion");
}

TEST(ModelConstructionTest, MoveAssignmentPreservesContainerMetadata) {
  auto source = Model::MakeContainer(MakeLeaf());
  auto destination = MakeLeaf("destination-model");

  destination = std::move(source);

  ASSERT_TRUE(destination.IsContainer());
  EXPECT_EQ(destination.Info().model_id, "test-model");
  EXPECT_EQ(destination.Info().task, "chat-completion");
}

TEST(ModelConstructionTest, MakeContainerRejectsContainerAsVariant) {
  auto nested = Model::MakeContainer(MakeLeaf());

  EXPECT_THROW(Model::MakeContainer(std::move(nested)), fl::Exception);
}

TEST(ModelConstructionTest, AddVariantRejectsContainerAndPreservesExistingSelection) {
  auto container = Model::MakeContainer(MakeLeaf());
  auto nested = Model::MakeContainer(MakeLeaf("nested-model"));

  EXPECT_THROW(container.AddVariant(std::move(nested)), fl::Exception);

  ASSERT_TRUE(container.IsContainer());
  EXPECT_EQ(container.Variants().size(), 1u);
  EXPECT_EQ(container.Info().model_id, "test-model");
}

TEST(ModelConstructionTest, ExecutionCaptureOwnsTheSelectedLeafAfterTheCatalogObjectIsDestroyed) {
  auto captured = [] {
    auto container = Model::MakeContainer(MakeLeaf("first"));
    container.AddVariant(MakeLeaf("second", "coding"));
    for (auto* variant : container.Variants()) {
      if (variant->Id() == "second") {
        container.SelectVariant(*variant);
      }
    }

    return container.CaptureExecutionModel();
  }();
  EXPECT_FALSE(captured.IsContainer());
  EXPECT_EQ(captured.Id(), "second");
  EXPECT_EQ(captured.Info().task, "coding");
}

TEST(ModelConstructionTest, ExecutionCapturePreservesCachedAndExternalRegistrationPolicies) {
  fl::test::FakeServiceBindings svc;
  ModelInfo info;
  info.model_id = "capture:1";
  auto downloaded = Model::FromModelInfo(info, "captured-cache", svc.download_manager, svc.model_load_manager);
  auto captured = downloaded.CaptureExecutionModel();
  EXPECT_TRUE(captured.IsCached());
  EXPECT_EQ(captured.GetPath(), downloaded.GetPath());

  auto external = Model::FromLocalRegistration(info, "", svc.download_manager, svc.model_load_manager);
  EXPECT_FALSE(external.CaptureExecutionModel().IsCached());
}

TEST(ModelConstructionTest, ExecutionCaptureRejectsRetiredCatalogSelections) {
  auto source = MakeLeaf();
  source.Deactivate();
  EXPECT_THROW(source.CaptureExecutionModel(), Exception);
}

TEST(ModelConstructionTest, CacheRefreshObservesCompletedRevisionWithoutDownloadingOrAdoptingPartialAssets) {
  static std::atomic<uint64_t> sequence{0};
  const auto scratch = std::filesystem::path(FOUNDRY_LOCAL_TEST_SCRATCH_DIR);
  ASSERT_TRUE(scratch.is_absolute());
  std::filesystem::create_directories(scratch);
  ASSERT_FALSE(std::filesystem::is_symlink(scratch));
  std::filesystem::path root;
  do {
    root = scratch / ("cache-refresh-" + std::to_string(test::CurrentPid()) + "-" + std::to_string(sequence++));
  } while (!std::filesystem::create_directory(root));

  ScopeGuard cleanup([&]() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  });

  test::FakeServiceBindings svc;
  svc.download_manager.SetCacheDirectory(root.string());
  ModelInfo info;
  info.model_id = "shared-download:1";
  auto model = Model::FromModelInfo(info, "", svc.download_manager, svc.model_load_manager);
  const auto model_path = root / "shared-download-1";
  const auto revision = model_path / "v1";
  std::filesystem::create_directories(revision);
  std::ofstream(revision / "genai_config.json") << "{}";
  model.RefreshCachedState();
  EXPECT_FALSE(model.IsCached());
  EXPECT_TRUE(model.GetPath().empty());

  std::ofstream(revision / "inference_model.json") << "{}";
  std::ofstream(model_path / "download.tmp") << "";
  model.RefreshCachedState();
  EXPECT_FALSE(model.IsCached());
  std::filesystem::remove(model_path / "download.tmp");
  model.RefreshCachedState();
  EXPECT_TRUE(model.IsCached());
  EXPECT_EQ(model.GetPath(), revision.string());
  EXPECT_EQ(model.CaptureExecutionModel().GetPath(), revision.string());
  EXPECT_THROW(model.RemoveFromCache((root / "replaced").string()), Exception);
  EXPECT_TRUE(model.IsCached());
  EXPECT_TRUE(std::filesystem::exists(revision / "inference_model.json"));
  model.ForgetCachedState();
  EXPECT_FALSE(model.IsCached());
  EXPECT_TRUE(model.GetPath().empty());
  EXPECT_TRUE(std::filesystem::exists(revision / "genai_config.json"));
  EXPECT_TRUE(std::filesystem::exists(revision / "inference_model.json"));
  model.RefreshCachedState();
  EXPECT_EQ(model.GetPath(), revision.string());
}
