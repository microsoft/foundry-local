// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "contracts/non_generative.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/predictive/non_generative_runtime.h"
#include "inferencing/predictive/non_generative_session.h"
#include "inferencing/predictive/session_executor.h"
#include "internal_api/test_helpers.h"
#include "items/text_item.h"
#include "model.h"
#include "service/non_generative_handlers.h"
#include "utils/safe_getenv.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace fl {

class ModelLoadManagerTestAccess {
 public:
  static std::shared_ptr<void> Acquire(
      ModelLoadManager& manager, const std::string& identity,
      uint64_t bytes, const std::string& readiness_group,
      const std::function<std::shared_ptr<void>()>& factory) {
    return manager.AcquireManagedRuntime(
        identity, identity, "/" + identity, bytes, readiness_group, factory);
  }

  static std::shared_ptr<void> AcquireArtifact(
      ModelLoadManager& manager, const std::string& identity,
      const std::string& model_id, const std::string& model_path,
      const std::function<std::shared_ptr<void>()>& factory) {
    std::shared_ptr<const GenAIConfig> config;
    const auto config_path =
        std::filesystem::path(model_path) / "genai_config.json";
    if (std::filesystem::is_regular_file(config_path))
      config = std::make_shared<GenAIConfig>(
          GenAIConfig::LoadFromFile(config_path.string()));
    return manager.AcquireManagedRuntime(
        identity, model_id, model_path, 1, "cpu", factory, {}, config);
  }

  static void Clear(ModelLoadManager& manager) {
    std::map<std::string, std::shared_ptr<ManagedModelRuntime>> removed;
    {
      std::lock_guard lock(manager.mutex_);
      removed.swap(manager.loaded_runtimes_);
    }
  }
};

namespace {

struct FakeExecutorSession {};

TEST(PredictiveSessionExecutorTest, QueuedCancellationSkipsWork) {
  detail::SessionExecutor<FakeExecutorSession> executor(
      [] { return std::make_unique<FakeExecutorSession>(); });
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  auto blocking = std::async(std::launch::async, [&] {
    executor.Invoke([&](FakeExecutorSession&) {
      entered.set_value();
      release_future.wait();
    });
  });
  entered.get_future().wait();

  std::atomic<bool> canceled{};
  std::atomic<bool> invoked{};
  auto queued = std::async(std::launch::async, [&] {
    EXPECT_THROW(
        executor.Invoke(
            [&](FakeExecutorSession&) { invoked = true; },
            [&] { return canceled.load(); }),
        std::runtime_error);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  canceled = true;
  queued.get();
  EXPECT_FALSE(invoked.load());

  release.set_value();
  blocking.get();
  executor.Invoke([&](FakeExecutorSession&) { EXPECT_FALSE(invoked.load()); });
}

TEST(PredictiveSessionExecutorTest, CancellationAfterExecutionWaitsForWork) {
  detail::SessionExecutor<FakeExecutorSession> executor(
      [] { return std::make_unique<FakeExecutorSession>(); });
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  std::atomic<bool> canceled{};
  auto running = std::async(std::launch::async, [&] {
    executor.Invoke(
        [&](FakeExecutorSession&) {
          entered.set_value();
          release_future.wait();
        },
        [&] { return canceled.load(); });
  });
  entered.get_future().wait();
  canceled = true;
  EXPECT_EQ(running.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  release.set_value();
  EXPECT_EQ(running.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
}

TEST(PredictiveSessionExecutorTest, RejectsBeyondQueueCapacity) {
  detail::SessionExecutor<FakeExecutorSession> executor(
      [] { return std::make_unique<FakeExecutorSession>(); });
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  auto blocking = std::async(std::launch::async, [&] {
    executor.Invoke([&](FakeExecutorSession&) {
      entered.set_value();
      release_future.wait();
    });
  });
  entered.get_future().wait();

  std::vector<std::future<void>> queued;
  queued.reserve(detail::kMaximumQueuedPredictiveRequests);
  for (size_t index = 0;
       index < detail::kMaximumQueuedPredictiveRequests; ++index) {
    queued.push_back(std::async(std::launch::async, [&] {
      executor.Invoke([](FakeExecutorSession&) {});
    }));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_THROW(executor.Invoke([](FakeExecutorSession&) {}),
               RuntimeCapacityError);

  release.set_value();
  blocking.get();
  for (auto& request : queued) request.get();
}

void WriteRankingBundle(const std::filesystem::path& path,
                        const std::string& model_id) {
  std::filesystem::create_directories(path / "components");
  std::ofstream(path / "genai_config.json")
      << R"({"model":{"type":"component","context_length":4096,"decoder":{"session_options":{"provider_options":[]}}}})";
  std::ofstream(path / "tokenizer.json") << "{}";
  std::ofstream(path / "tokenizer_config.json") << "{}";
  nlohmann::json components = nlohmann::json::object();
  for (const auto& [name, role] :
       {std::pair{"encoder", "backbone"}, std::pair{"state_head", "head"},
        std::pair{"action_head", "head"}, std::pair{"scorer", "scorer"}}) {
    const auto relative = "components/" + std::string(name) + ".onnx";
    std::ofstream(path / relative) << "synthetic";
    components[name] = {{"role", role}, {"filename", relative}};
  }
  std::ofstream(path / "component_manifest.json")
      << nlohmann::json{{"schema_version", 1},
                        {"model_type", "synthetic"},
                        {"components", components}};
  std::ofstream(path / "inference_model.json")
      << nlohmann::json{
             {"Name", model_id},
             {"Alias", "clm"},
             {"Task", "text-ranking"},
             {"ComponentManifest", "component_manifest.json"},
             {"License", "MIT"},
             {"Provenance",
              {{"source", "synthetic"},
               {"artifact_revision", "revision"},
               {"base_model", "synthetic/base"},
               {"base_revision", "revision"}}},
             {"Provider",
              {{"execution_provider", "cpu"}, {"variant", "fp32"}}},
             {"Capabilities", nlohmann::json::array({"structured-input"})}};
}

class FakeRankingRuntime final : public IRankingRuntime {
 public:
  nlohmann::json Rank(
      const RankingRequest& request,
      std::function<bool()> is_canceled) override {
    if (is_canceled && is_canceled())
      throw std::runtime_error("canceled");
    last_request = request;
    return {{"ranked", nlohmann::json::array()}};
  }

  RankingRequest last_request;
};

TEST(ManagedModelRuntimeTest, SameKeyColdLoadsAreCoalesced) {
  test::CpuOnlyEpDetector detector;
  ModelLoadManager manager(detector, test::NullLog());
  std::atomic<int> constructions{0};
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  auto factory = [&]() -> std::shared_ptr<void> {
    ++constructions;
    entered.set_value();
    release_future.wait();
    return std::make_shared<int>(42);
  };

  auto first = std::async(std::launch::async, [&] {
    return ModelLoadManagerTestAccess::Acquire(
        manager, "same", 1, "cpu", factory);
  });
  entered.get_future().wait();
  auto second = std::async(std::launch::async, [&] {
    return ModelLoadManagerTestAccess::Acquire(
        manager, "same", 1, "cpu", factory);
  });

  EXPECT_EQ(first.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  EXPECT_EQ(second.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  release.set_value();
  EXPECT_EQ(first.get(), second.get());
  EXPECT_EQ(constructions.load(), 1);
}

TEST(ManagedModelRuntimeTest, FailedColdLoadIsRemovedAndCanBeRetried) {
  test::CpuOnlyEpDetector detector;
  ModelLoadManager manager(detector, test::NullLog());
  std::atomic<int> constructions{0};
  auto factory = [&]() -> std::shared_ptr<void> {
    if (++constructions == 1) throw std::runtime_error("synthetic failure");
    return std::make_shared<int>(42);
  };

  EXPECT_THROW(ModelLoadManagerTestAccess::Acquire(
                   manager, "retry", 1, "cpu", factory),
               std::runtime_error);
  EXPECT_NE(ModelLoadManagerTestAccess::Acquire(
                manager, "retry", 1, "cpu", factory),
            nullptr);
  EXPECT_EQ(constructions.load(), 2);
}

TEST(ManagedModelRuntimeTest,
     LoadingIsBusyButNotLoadedAndBlocksRetirement) {
  test::FakeServiceBindings service;
  const auto root =
      test::TempPath::CreateTempDir("fl_loading_runtime_");
  ModelInfo info;
  info.model_id = "loading:1";
  info.name = "loading";
  info.alias = "loading";
  info.task = "text-ranking";
  auto model = Model::FromLocalRegistration(
      std::move(info), root.string(), service.download_manager,
      service.model_load_manager);
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  auto loading = std::async(std::launch::async, [&] {
    return ModelLoadManagerTestAccess::AcquireArtifact(
        service.model_load_manager, "loading-key", model.Id(), root.string(),
        [&]() -> std::shared_ptr<void> {
          entered.set_value();
          release_future.wait();
          return std::make_shared<int>(42);
        });
  });
  entered.get_future().wait();

  EXPECT_FALSE(model.IsLoaded());
  EXPECT_TRUE(service.model_load_manager.IsModelLoading(model));
  EXPECT_THROW(
      service.model_load_manager.RunWithUnloadedModelsInDirectory(
          root.string(), [] {}),
      std::exception);
  EXPECT_THROW(model.BeginUnregister(), std::exception);

  release.set_value();
  auto lease = loading.get();
  EXPECT_TRUE(model.IsLoaded());
  lease.reset();
  EXPECT_TRUE(
      service.model_load_manager.UnloadNonGenerativeRuntime(model));
}

TEST(ManagedModelRuntimeTest, RejectsLoadsBeyondMemoryBudget) {
  test::CpuOnlyEpDetector detector;
  ModelLoadManager manager(detector, test::NullLog(), 100, 1);
  auto factory = []() -> std::shared_ptr<void> {
    return std::make_shared<int>(42);
  };

  EXPECT_NE(ModelLoadManagerTestAccess::Acquire(
                manager, "first", 80, "cpu", factory),
            nullptr);
  EXPECT_THROW(ModelLoadManagerTestAccess::Acquire(
                   manager, "second", 21, "cpu", factory),
               std::runtime_error);
}

TEST(ManagedModelRuntimeTest, ArtifactOperationsIncludeTypedRuntimes) {
  test::FakeServiceBindings service;
  const auto root =
      test::TempPath::CreateTempDir("fl_typed_artifact_");
  std::ofstream(root.path() / "genai_config.json")
      << R"({"model":{"type":"component","context_length":4096}})";
  const auto canonical =
      std::filesystem::weakly_canonical(root.path()).string();
  auto lease = ModelLoadManagerTestAccess::AcquireArtifact(
      service.model_load_manager, "artifact-key", "typed:1", canonical,
      []() -> std::shared_ptr<void> { return std::make_shared<int>(42); });
  lease.reset();

  EXPECT_TRUE(service.model_load_manager.IsModelArtifactLoaded(
      "typed:1", canonical));
  const auto state = service.model_load_manager.GetLoadedModelState(
      "typed:1", canonical);
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->model_path, canonical);
  EXPECT_EQ(state->context_length, 4096u);
  EXPECT_TRUE(service.model_load_manager.UnloadModelArtifact(
      "typed:1", canonical));
  EXPECT_FALSE(service.model_load_manager.IsModelArtifactLoaded(
      "typed:1", canonical));
}

TEST(ManagedModelRuntimeTest, RetiredHandleDoesNotObserveReplacementRuntime) {
  test::FakeServiceBindings service;
  const auto root =
      test::TempPath::CreateTempDir("fl_replaced_artifact_");
  ModelInfo old_info;
  old_info.model_id = "typed:1";
  old_info.name = "typed";
  old_info.alias = "typed";
  old_info.task = "text-ranking";
  auto old_model = Model::FromLocalRegistration(
      old_info, root.string(), service.download_manager,
      service.model_load_manager);
  auto replacement = Model::FromLocalRegistration(
      std::move(old_info), root.string(), service.download_manager,
      service.model_load_manager);
  old_model.Deactivate();
  const auto canonical =
      std::filesystem::weakly_canonical(root.path()).string();
  auto lease = ModelLoadManagerTestAccess::AcquireArtifact(
      service.model_load_manager, "replacement-key", "typed:1", canonical,
      []() -> std::shared_ptr<void> { return std::make_shared<int>(42); });
  lease.reset();

  EXPECT_FALSE(old_model.IsLoaded());
  EXPECT_TRUE(replacement.IsLoaded());
}

TEST(ManagedModelRuntimeTest, ReleasesAdmissionAfterRuntimeDestruction) {
  test::CpuOnlyEpDetector detector;
  ModelLoadManager manager(detector, test::NullLog(), 100, 1);
  auto factory = []() -> std::shared_ptr<void> {
    return std::make_shared<int>(42);
  };

  auto lease = ModelLoadManagerTestAccess::Acquire(
      manager, "first", 80, "cpu", factory);
  ModelLoadManagerTestAccess::Clear(manager);
  EXPECT_THROW(ModelLoadManagerTestAccess::Acquire(
                   manager, "second", 21, "cpu", factory),
               std::runtime_error);
  lease.reset();
  EXPECT_NE(ModelLoadManagerTestAccess::Acquire(
                manager, "second", 21, "cpu", factory),
            nullptr);
}

TEST(ManagedModelRuntimeTest, ActiveLeaseBlocksExplicitUnload) {
  test::FakeServiceBindings service;
  ModelInfo info;
  info.model_id = "typed:1";
  info.name = "typed";
  info.alias = "typed";
  info.task = "text-ranking";
  auto model = Model::FromLocalRegistration(
      std::move(info), "/typed:1", service.download_manager,
      service.model_load_manager);
  auto lease = ModelLoadManagerTestAccess::Acquire(
      service.model_load_manager, "typed:1", 1, "cpu",
      []() -> std::shared_ptr<void> { return std::make_shared<int>(42); });

  EXPECT_THROW(service.model_load_manager.UnloadNonGenerativeRuntime(model),
               std::exception);
  lease.reset();
  EXPECT_TRUE(
      service.model_load_manager.UnloadNonGenerativeRuntime(model));
}

TEST(ManagedModelRuntimeTest, ShutdownRejectsNewAcquisitions) {
  test::CpuOnlyEpDetector detector;
  ModelLoadManager manager(detector, test::NullLog());
  manager.RejectNewLoads();

  EXPECT_THROW(
      ModelLoadManagerTestAccess::Acquire(
          manager, "shutdown", 1, "cpu",
          []() -> std::shared_ptr<void> {
            return std::make_shared<int>(42);
          }),
      std::exception);
}

TEST(ManagedModelRuntimeTest, ShutdownCancelsAndDrainsPredictiveRequests) {
  test::CpuOnlyEpDetector detector;
  ModelLoadManager manager(detector, test::NullLog());
  bool canceled = false;
  int session_identity;
  manager.RegisterPredictiveSession(
      &session_identity, [&] { canceled = true; }, [] {});
  std::optional<ModelLoadManager::PredictiveRequestLease> active(
      manager.AcquirePredictiveRequest());

  manager.RejectNewLoads();
  EXPECT_TRUE(canceled);
  manager.UnloadAll(std::chrono::milliseconds(1));
  auto destruction_wait = std::async(std::launch::async, [&] {
    manager.WaitForPredictiveRequests();
  });
  EXPECT_EQ(destruction_wait.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  active.reset();
  EXPECT_EQ(destruction_wait.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  manager.UnregisterPredictiveSession(&session_identity);
}

TEST(ManagedModelRuntimeTest, SerializesReadinessWithinProviderGroup) {
  test::CpuOnlyEpDetector detector;
  ModelLoadManager manager(detector, test::NullLog(), 1000, 1);
  std::promise<void> first_entered;
  std::promise<void> release_first;
  auto release_future = release_first.get_future().share();
  auto first = std::async(std::launch::async, [&] {
    return ModelLoadManagerTestAccess::Acquire(
        manager, "first", 1, "cuda", [&]() -> std::shared_ptr<void> {
          first_entered.set_value();
          release_future.wait();
          return std::make_shared<int>(1);
        });
  });
  first_entered.get_future().wait();
  std::atomic<bool> second_entered{false};
  auto second = std::async(std::launch::async, [&] {
    return ModelLoadManagerTestAccess::Acquire(
        manager, "second", 1, "cuda", [&]() -> std::shared_ptr<void> {
          second_entered = true;
          return std::make_shared<int>(2);
        });
  });
  EXPECT_EQ(second.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  EXPECT_FALSE(second_entered.load());

  auto cpu = ModelLoadManagerTestAccess::Acquire(
      manager, "cpu", 1, "cpu",
      []() -> std::shared_ptr<void> { return std::make_shared<int>(3); });
  EXPECT_NE(cpu, nullptr);
  release_first.set_value();
  EXPECT_NE(first.get(), nullptr);
  EXPECT_NE(second.get(), nullptr);
  EXPECT_TRUE(second_entered.load());
}

TEST(NonGenerativeSessionTest,
     DispatchesAndMergesOptionsWithoutRealModelWeights) {
  test::FakeServiceBindings service;
  const auto root =
      test::TempPath::CreateTempDir("fl_predictive_session_");
  const auto path = root.string();
  constexpr std::string_view model_id = "clm-generic-cpu:1";
  WriteRankingBundle(path, std::string(model_id));

  ModelInfo info;
  info.model_id = model_id;
  info.name = "clm-generic-cpu";
  info.alias = "clm";
  info.task = "text-ranking";
  auto model = Model::FromLocalRegistration(
      std::move(info), path, service.download_manager,
      service.model_load_manager);
  const auto io = model.GetInputOutputInfo();
  ASSERT_EQ(io.num_inputs, 1u);
  ASSERT_EQ(io.num_outputs, 1u);
  EXPECT_EQ(io.inputs[0]->type, FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(static_cast<const TextItem*>(io.inputs[0])->text_type,
            FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);
  EXPECT_EQ(io.outputs[0]->type, FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(static_cast<const TextItem*>(io.outputs[0])->text_type,
            FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);
  EXPECT_THROW(
      NonGenerativeSession(model, service.model_load_manager, service.logger,
                           service.telemetry),
      std::exception);
  EXPECT_FALSE(model.IsLoaded());
  const auto canonical = std::filesystem::weakly_canonical(path).string();
  const auto identity =
      "non-generative|" + std::string(model_id) + "|" + canonical +
      "||text-ranking";
  auto fake = std::make_shared<FakeRankingRuntime>();
  auto seeded = ModelLoadManagerTestAccess::AcquireArtifact(
      service.model_load_manager, identity, std::string(model_id), canonical,
      [fake]() -> std::shared_ptr<void> { return fake; });
  seeded.reset();

  {
    auto session = std::make_unique<NonGenerativeSession>(
        model, service.model_load_manager, service.logger, service.telemetry);
    auto second_session = std::make_unique<NonGenerativeSession>(
        model, service.model_load_manager, service.logger, service.telemetry);
    KeyValuePairs defaults;
    defaults.Add("temperature", "0.5");
    session->SetSessionOptions(defaults);

    Request request;
    request.AddOwnedItem(std::make_unique<TextItem>(
        nlohmann::json{{"answers", {"a", "b"}}}.dump(),
        FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
    request.options.Add("temperature", "0.75");
    Response response;
    session->ProcessRequest(request, response);
    EXPECT_FLOAT_EQ(fake->last_request.temperature, 0.75f);
    ASSERT_EQ(response.items.size(), 1u);

    Request mismatched;
    mismatched.AddOwnedItem(std::make_unique<TextItem>(
        nlohmann::json{{"model", "other"}, {"answers", {"a", "b"}}}.dump(),
        FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
    Response ignored;
    EXPECT_THROW(session->ProcessRequest(mismatched, ignored), std::exception);
    EXPECT_THROW(
        service.model_load_manager.UnloadNonGenerativeRuntime(model),
        std::exception);
    session.reset();
    EXPECT_THROW(
        service.model_load_manager.UnloadNonGenerativeRuntime(model),
        std::exception);
    second_session.reset();
  }

  EXPECT_TRUE(service.model_load_manager.UnloadNonGenerativeRuntime(model));
}

TEST(NonGenerativeSessionTest, RejectsPackageTaskMismatchBeforeAcquisition) {
  test::FakeServiceBindings service;
  const auto root =
      test::TempPath::CreateTempDir("fl_predictive_task_mismatch_");
  WriteRankingBundle(root.string(), "clm-generic-cpu:1");
  ModelInfo info;
  info.model_id = "clm-generic-cpu:1";
  info.name = "clm-generic-cpu";
  info.alias = "clm";
  info.task = "typed-decision";
  auto model = Model::FromLocalRegistration(
      std::move(info), root.string(), service.download_manager,
      service.model_load_manager);

  EXPECT_THROW(
      NonGenerativeSession(model, service.model_load_manager, service.logger,
                           service.telemetry),
      std::exception);
  EXPECT_FALSE(model.IsLoaded());
}

TEST(NonGenerativeRuntimeTest, ExportedPackagesRunWhenConfigured) {
  const auto root_value = test::SafeGetEnv("FOUNDRY_LOCAL_NON_GENERATIVE_TEST_ROOT");
  if (root_value.empty()) {
    GTEST_SKIP() << "set FOUNDRY_LOCAL_NON_GENERATIVE_TEST_ROOT to run exported packages";
  }
  const std::filesystem::path root(root_value);
  const auto clm = root / "clm-v0.1-8b-fp32";
  const auto kev = root / "kev-4b-fp32";
  if (!std::filesystem::is_directory(clm) || !std::filesystem::is_directory(kev)) {
    GTEST_SKIP() << "configured root does not contain CLM and KEV FP32 packages";
  }

  RankingRequest ranking;
  ranking.context = {{"weather", "heavy rain"}};
  ranking.question = "Which activity is more suitable?";
  ranking.answers = {"Have a picnic outdoors", "Visit an indoor museum"};
  auto ranked = RankingRuntime(clm.string()).Rank(ranking);
  ASSERT_EQ(ranked.at("ranked").size(), 2u);
  EXPECT_EQ(ranked.at("ranked").at(0).at("rank"), 1);
  for (size_t index = 0; index < ranked.at("ranked").size(); ++index) {
    const auto& item = ranked.at("ranked").at(index);
    EXPECT_TRUE(item.contains("id"));
    EXPECT_TRUE(item.contains("index"));
    EXPECT_EQ(item.at("candidate"),
              ranking.answers.at(item.at("index").get<size_t>()));
  }

  auto decision = nlohmann::ordered_json{
      {"state", {{"weather", "heavy rain"}}},
      {"questions",
       {{"umbrella",
         {{"type", "noul"}, {"instructions", "Should I take an umbrella?"}}}}},
  }
                      .get<NonGenerativeRequest>();
  auto answers = DecisionRuntime(kev.string()).Decide(decision);
  ASSERT_TRUE(answers.contains("umbrella"));
  EXPECT_EQ(answers.at("umbrella").at("type"), "noul");
  EXPECT_GE(answers.at("umbrella").at("noul").get<double>(), 0.0);
  EXPECT_LE(answers.at("umbrella").at("noul").get<double>(), 1.0);
}

}  // namespace
}  // namespace fl
