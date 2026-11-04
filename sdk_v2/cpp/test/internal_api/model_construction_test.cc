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
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
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

TEST(ModelConstructionTest, HostedPreservationIsSharedOnlyByMatchingModelIdsAndPaths) {
  test::FakeServiceBindings services;
  auto info = MakeLeaf("shared-model:1").Info();
  auto make_model = [&](const ModelInfo& metadata, const std::string& path) {
    return Model::FromModelInfo(metadata, path, services.download_manager, services.model_load_manager);
  };
  auto model = Model::MakeContainer(make_model(info, "original"));
  auto matching = make_model(info, "original");
  auto other_path = make_model(info, "replacement");
  auto other_info = info;
  other_info.model_id = "shared-model:2";
  other_info.version = 2;
  auto other_id = make_model(other_info, "original");

  EXPECT_FALSE(model.PreserveLoadedSelection(true));
  model.AddVariant(make_model(other_info, "replacement"));
  EXPECT_FALSE(other_path.PreserveLoadedSelection(false));
  EXPECT_FALSE(other_id.PreserveLoadedSelection(false));
  EXPECT_FALSE(model.SelectDefaultVariant({}, true));
  EXPECT_EQ(model.Id(), info.model_id);
  EXPECT_TRUE(matching.PreserveLoadedSelection(false));
  EXPECT_TRUE(model.SelectDefaultVariant({}, true));
  EXPECT_EQ(model.Id(), other_info.model_id);
}

TEST(ModelConstructionTest, BorrowedPathsRemainImmutableDuringConcurrentCacheRefresh) {
  test::FakeServiceBindings services;
  const std::string first = "cache-" + std::string(100, 'a');
  const std::string second = "cache-" + std::string(100, 'b');
  auto model = Model::FromModelInfo(MakeLeaf().Info(), first, services.download_manager, services.model_load_manager);
  const auto* original = model.GetPath().c_str();
  model.UpdateCachedState(first);
  EXPECT_EQ(model.GetPath().c_str(), original);

  std::atomic<bool> done{false};
  std::atomic<bool> valid{true};
  std::thread reader([&] {
    do {
      const auto& path = model.GetPath();
      if (path != first && path != second) {
        valid.store(false);
      }
    } while (!done.load());
  });
  for (int i = 0; i < 500; ++i) {
    model.UpdateCachedState(i % 2 == 0 ? second : first);
  }
  done.store(true);
  reader.join();
  EXPECT_TRUE(valid.load());
  EXPECT_STREQ(original, first.c_str());
  model.UpdateCachedState({});
  EXPECT_TRUE(model.GetPath().empty());
  EXPECT_STREQ(original, first.c_str());
}

TEST(ModelConstructionTest, MoveAssignmentRetainsBorrowedPathsFromBothModels) {
  auto source = MakeLeaf("source");
  auto destination = MakeLeaf("destination");
  source.UpdateCachedState(std::string(100, 's'));
  destination.UpdateCachedState(std::string(100, 'd'));
  const auto* source_path = source.GetPath().c_str();
  const auto* destination_path = destination.GetPath().c_str();
  destination = std::move(source);
  EXPECT_STREQ(source_path, std::string(100, 's').c_str());
  EXPECT_STREQ(destination_path, std::string(100, 'd').c_str());
  EXPECT_EQ(destination.GetPath().c_str(), source_path);
}

TEST(ModelConstructionTest, ConcurrentSelectionReservationsRollbackWithoutReleasingAnotherLoad) {
  test::FakeServiceBindings services;
  auto model = Model::MakeContainer(
      Model::FromModelInfo(MakeLeaf().Info(), "original", services.download_manager, services.model_load_manager));
  std::unique_ptr<LoadedSelectionReservation> first;
  std::unique_ptr<LoadedSelectionReservation> second;
  auto first_capture = model.CaptureExecutionModel(&first);
  auto second_capture = model.CaptureExecutionModel(&second);
  auto newer = MakeLeaf().Info();
  newer.version = 2;
  newer.model_id = "newer";
  model.AddVariant(Model::FromModelInfo(newer, "replacement", services.download_manager, services.model_load_manager));
  first.reset();
  EXPECT_FALSE(model.SelectDefaultVariant({}, true));
  EXPECT_EQ(model.Id(), first_capture.Id());
  second->Commit();
  second.reset();
  EXPECT_FALSE(model.SelectDefaultVariant({}, true));
  EXPECT_FALSE(model.IsLoaded());
  EXPECT_TRUE(second_capture.PreserveLoadedSelection(false));
  EXPECT_TRUE(model.SelectDefaultVariant({}, true));
  EXPECT_EQ(model.Id(), newer.model_id);

  std::unique_ptr<LoadedSelectionReservation> failed;
  auto failed_capture = model.CaptureExecutionModel(&failed);
  failed.reset();
  EXPECT_FALSE(failed_capture.PreserveLoadedSelection(false));
}

TEST(ModelConstructionTest, PeerReconciliationCannotReleaseAnInFlightOrNewlyCommittedSelection) {
  test::FakeServiceBindings services;
  auto model = Model::FromModelInfo(MakeLeaf().Info(), "artifact", services.download_manager, services.model_load_manager);
  std::unique_ptr<LoadedSelectionReservation> reservation;
  auto captured = model.CaptureExecutionModel(&reservation);
  int observations = 0;
  services.model_load_manager.ReconcileLoadedSelections([&](const auto&, const auto&) {
    ++observations;
    return false;
  });
  EXPECT_EQ(observations, 0);
  reservation->Commit();
  reservation.reset();

  services.model_load_manager.ReconcileLoadedSelections([&](const auto&, const auto&) {
    captured = model.CaptureExecutionModel(&reservation);
    return false;
  });
  EXPECT_TRUE(model.PreserveLoadedSelection(false));
  reservation->Commit();
  reservation.reset();

  services.model_load_manager.ReconcileLoadedSelections([&](const auto&, const auto&) {
    captured = model.CaptureExecutionModel(&reservation);
    reservation->Commit();
    reservation.reset();
    return false;
  });
  EXPECT_TRUE(model.PreserveLoadedSelection(true));
  services.model_load_manager.ReconcileLoadedSelections([](const auto&, const auto&) { return false; });
  EXPECT_FALSE(model.PreserveLoadedSelection(false));
}

class ModelLifecycleDeathTest : public testing::TestWithParam<bool> {};

TEST(ModelLifecycleDeathTest, CaptureReservesTheSelectedArtifactBeforeConcurrentDefaultRefresh) {
  ASSERT_EXIT(
      {
        std::thread watchdog([] {
          std::this_thread::sleep_for(std::chrono::seconds(5));
          std::_Exit(2);
        });
        test::FakeServiceBindings services;
        auto cpu = MakeLeaf("model-generic-cpu:1").Info();
        auto model = Model::MakeContainer(
            Model::FromModelInfo(cpu, "original", services.download_manager, services.model_load_manager));
        auto* leaf = model.Variants().front();
        leaf->BeginUnregister();
        std::unique_ptr<LoadedSelectionReservation> reservation;
        auto capturing = std::async(std::launch::async, [&] { return model.CaptureExecutionModel(&reservation); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto gpu = MakeLeaf("model-cuda-gpu:1").Info();
        model.AddVariant(Model::FromModelInfo(gpu, "replacement", services.download_manager, services.model_load_manager));
        if (model.SelectDefaultVariant({}, true) || model.Id() != cpu.model_id) {
          std::_Exit(3);
        }

        leaf->EndUnregister();
        auto captured = capturing.get();
        if (captured.Id() != cpu.model_id || model.SelectDefaultVariant({}, true)) {
          std::_Exit(4);
        }

        leaf->UpdateCachedState("changed-after-capture");
        if (leaf->LocalPathSnapshot() != "original") {
          std::_Exit(5);
        }

        reservation.reset();
        if (!model.SelectDefaultVariant({}, true) || model.Id() != gpu.model_id) {
          std::_Exit(6);
        }
        std::_Exit(0);
      },
      testing::ExitedWithCode(0), "");
}

TEST_P(ModelLifecycleDeathTest, AliasOperationDoesNotHoldGroupStateWhileWaitingForUnregister) {
  ASSERT_EXIT(
      {
        std::thread watchdog([] {
          std::this_thread::sleep_for(std::chrono::seconds(5));
          std::_Exit(2);
        });
        auto model = Model::MakeContainer(MakeLeaf());
        model.BeginUnregister();
        const bool load = GetParam();
        std::promise<void> started;
        auto operation = std::async(std::launch::async, [&] {
          started.set_value();
          try {
            if (load) {
              model.Load();
            } else {
              model.Unload();
            }
            return false;
          } catch (const Exception&) {
            return true;
          }
        });
        started.get_future().wait();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // Unregister owns the lifecycle locks before it enumerates and retires the group.
        if (model.Variants().size() != 1) {
          std::_Exit(3);
        }

        model.Deactivate();
        model.EndUnregister();
        std::_Exit(operation.get() == load ? 0 : 4);
      },
      testing::ExitedWithCode(0), "");
}

INSTANTIATE_TEST_SUITE_P(LoadAndUnload, ModelLifecycleDeathTest, testing::Bool(),
                         [](const testing::TestParamInfo<bool>& info) { return info.param ? "Load" : "Unload"; });

TEST(ModelLifecycleDeathTest, DefaultRefreshKeepsTheArtifactWhoseLoadIsInProgress) {
  ASSERT_EXIT(
      {
        std::thread watchdog([] {
          std::this_thread::sleep_for(std::chrono::seconds(5));
          std::_Exit(2);
        });
        class BlockingLogger final : public ILogger {
         public:
          void Log(LogLevel, std::string_view message) override {
            if (message.starts_with("loading model from ")) {
              entered.set_value();
              release.get_future().wait();
            }
          }
          std::promise<void> entered;
          std::promise<void> release;
        } logger;
        test::FakeServiceBindings services;
        ModelLoadManager loader(services.ep_detector, logger);
        auto cpu = MakeLeaf("model-generic-cpu:1").Info();
        const auto path = (std::filesystem::path(FOUNDRY_LOCAL_TEST_DATA_DIR) / "tiny-paged-attention").string();
        auto model =
            Model::MakeContainer(Model::FromModelInfo(cpu, path, services.download_manager, loader));
        auto loading = std::async(std::launch::async, [&] { model.Load(); });
        logger.entered.get_future().wait();
        auto gpu = MakeLeaf("model-generic-gpu:1").Info();
        model.AddVariant(Model::FromModelInfo(gpu, path, services.download_manager, loader));
        if (model.SelectDefaultVariant({}, true) || model.Id() != cpu.model_id) {
          std::_Exit(3);
        }

        logger.release.set_value();
        loading.get();
        if (!model.IsLoaded()) {
          std::_Exit(4);
        }

        model.Unload();
        if (!model.SelectDefaultVariant({}, true) || model.Id() != "model-generic-gpu:1") {
          std::_Exit(5);
        }

        std::_Exit(0);
      },
      testing::ExitedWithCode(0), "");
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
