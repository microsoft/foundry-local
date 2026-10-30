// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "api/backend.h"
#include "c_api_types.h"
#include "exception.h"
#include "ep_detection/ep_types.h"
#include "hosting/host_objects.h"
#include "internal_api/c_api_test_helpers.h"
#include "items/text_item.h"

#include <memory>
#include <string>

namespace {

struct Calls {
  int loads = 0;
  int unloads = 0;
  int sessions = 0;
  int released_sessions = 0;
  int released_preflights = 0;
  int released_managers = 0;
  int preflights = 0;
  bool reject_load = false;
  flStreamingCallback callback = nullptr;
  void* callback_context = nullptr;
  std::string option;
  std::string tool;
  std::function<void()> on_session_release;
};

class RecordingPreflight final : public fl::api::PreflightBackend {
 public:
  explicit RecordingPreflight(Calls& calls, fl::api::BackendKind kind = fl::api::BackendKind::Hosted)
      : PreflightBackend(kind), calls_(calls) {}
  ~RecordingPreflight() override { ++calls_.released_preflights; }
  fl::RequestBudget Execute() override {
    ++calls_.preflights;
    return {7, 3, 10, 64, true, 0};
  }

 private:
  Calls& calls_;
};

class RecordingSession final : public fl::api::SessionBackend {
 public:
  explicit RecordingSession(Calls& calls, fl::api::BackendKind kind = fl::api::BackendKind::Hosted)
      : SessionBackend(kind), calls_(calls) {}
  ~RecordingSession() override {
    ++calls_.released_sessions;
    if (calls_.on_session_release) {
      calls_.on_session_release();
    }
  }

  void SetStreamingCallback(flStreamingCallback callback, void* context) override {
    calls_.callback = callback;
    calls_.callback_context = context;
  }

  void SetSessionOptions(const fl::KeyValuePairs& options) override {
    const auto* value = options.Find("max_output_tokens");
    calls_.option = value ? value : "";
  }

  size_t ProcessRequest(const fl::Request& request, fl::Response& response,
                        std::optional<fl::StreamingCallback> = std::nullopt) override {
    if (request.items.size() != 1 || request.items.front()->type != FOUNDRY_LOCAL_ITEM_TEXT) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "expected one local text item");
    }

    response.items.push_back(
        std::make_unique<fl::TextItem>(static_cast<const fl::TextItem&>(*request.items.front()).text + ":hosted"));
    response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;
    response.usage = {7, 3, 10};
    return 2;
  }

  void AddToolDefinition(fl::ToolDefinition definition) override { calls_.tool = std::move(definition.name); }
  bool RemoveToolDefinition(const std::string& name) override {
    if (calls_.tool != name) {
      return false;
    }

    calls_.tool.clear();
    return true;
  }

  size_t TurnCount() const override { return 2; }
  size_t UndoTurns(size_t) override { FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "remote session is busy"); }
  std::unique_ptr<fl::api::PreflightBackend> CreateRequestPreflight(const fl::Request&) const override {
    return std::make_unique<RecordingPreflight>(calls_);
  }

 private:
  Calls& calls_;
};

class RecordingModel final : public fl::api::ModelBackend {
 public:
  explicit RecordingModel(Calls& calls) : ModelBackend(fl::api::BackendKind::Hosted), calls_(calls) {
    info_.model_id = "hosted-model:1";
  }

  const fl::ModelInfo& Info() const override { return info_; }
  fl::api::InputOutputInfo GetInputOutputInfo() const override { return {nullptr, 0, nullptr, 0}; }
  bool IsCached() const override { return true; }
  void RefreshCachedState() override {}
  void ForgetCachedState() override {}
  const std::string& GetPath() const override { return path_; }
  void Download(std::function<int(float)>) override {}
  bool IsLoaded() const override { return calls_.loads > calls_.unloads; }
  void Load(std::optional<std::string_view> = std::nullopt) override {
    if (calls_.reject_load) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK, "host connection was lost");
    }

    ++calls_.loads;
  }

  void Unload() override { ++calls_.unloads; }
  void RemoveFromCache(std::optional<std::string_view> = std::nullopt) override {}
  std::vector<fl::api::ModelBackend*> Variants() const override { return {const_cast<RecordingModel*>(this)}; }

  void SelectVariant(const ModelBackend&) override {}
  std::unique_ptr<fl::api::SessionBackend> CreateSession() const override {
    ++calls_.sessions;
    return std::make_unique<RecordingSession>(calls_);
  }

 private:
  Calls& calls_;
  fl::ModelInfo info_;
  std::string path_ = "hosted-model";
};

class RecordingManager final : public fl::api::ManagerBackend {
 public:
  explicit RecordingManager(Calls& calls) : ManagerBackend(fl::api::BackendKind::Embedded), calls_(calls) {}
  ~RecordingManager() override { ++calls_.released_managers; }
  fl::api::CatalogBackend& GetCatalog(fl::CatalogType) override {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED, "catalog is not used by this test");
  }
  std::unique_ptr<fl::api::ModelBackend> CaptureModel(const fl::api::ModelBackend&) override {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED, "model capture is not used by this test");
  }
  bool IsModelArtifactLoaded(const std::string&, const std::string&) const override { return false; }
  void UnloadModelArtifact(const std::string&, const std::string&) override {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED, "model unloading is not used by this test");
  }
  void StartWebService() override {}
  const std::vector<std::string>& GetWebServiceUrls() const override { return urls_; }
  void StopWebService() override {}
  std::span<const flEpInfo> GetDiscoverableEps() const override { return {}; }
  fl::api::DeviceProviders GetRuntimeDevices() const override { return {}; }
  void SetCatalogRuntimeDevices(fl::api::DeviceProviders) override {}
  fl::EpDownloadResult DownloadAndRegisterEps(const std::vector<std::string>*,
                                              const fl::api::EpProgressCallback&) override {
    return {};
  }
  bool IsEpDownloadInProgress() const override { return false; }
  void Shutdown() override { shutdown_ = true; }
  bool IsShutdownRequested() const override { return shutdown_; }

 private:
  Calls& calls_;
  std::vector<std::string> urls_;
  bool shutdown_ = false;
};

TEST(ApiBackendTest, ModelHandleDispatchesToHostedAdapterWithoutNativeModel) {
  const auto* api = fl::test::GetApi();
  const auto* models = api->GetModelApi();
  Calls calls;
  RecordingModel backend(calls);
  auto* model = AsHandle<flModel>(&backend);

  const flModelInfo* info = nullptr;
  ASSERT_FL_OK(api, models->GetInfo(model, &info));
  EXPECT_STREQ(models->Info_GetId(info), "hosted-model:1");
  ASSERT_FL_OK(api, models->Load(model));
  EXPECT_EQ(calls.loads, 1);
  int loaded = 0;
  ASSERT_FL_OK(api, models->IsLoaded(model, &loaded));
  EXPECT_EQ(loaded, 1);

  ASSERT_FL_OK(api, models->Unload(model));
  EXPECT_EQ(calls.unloads, 1);
  ASSERT_FL_OK(api, models->IsLoaded(model, &loaded));
  EXPECT_EQ(loaded, 0);

  calls.reject_load = true;
  fl::test::StatusGuard failure{models->Load(model), api};
  ASSERT_NE(failure.s, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(failure.s), FOUNDRY_LOCAL_ERROR_NETWORK);
  EXPECT_TRUE(std::string(api->Status_GetErrorMessage(failure.s)).ends_with("host connection was lost"));
  EXPECT_EQ(calls.loads, 1);
}

TEST(ApiBackendTest, SessionAdapterPreservesLocalValuesAndDispatchesStatefulOperations) {
  const auto* api = fl::test::GetApi();
  const auto* inference = api->GetInferenceApi();
  Calls calls;
  RecordingModel model(calls);
  flSession* session = nullptr;
  ASSERT_FL_OK(api, inference->Session_Create(AsHandle<flModel>(&model), &session));
  std::unique_ptr<flSession, decltype(inference->Session_Release)> owner(session, inference->Session_Release);
  EXPECT_EQ(calls.sessions, 1);

  fl::Request request;
  request.AddOwnedItem(std::make_unique<fl::TextItem>("local input"));
  fl::Response response;
  auto* response_handle = AsHandle<flResponse>(&response);
  ASSERT_FL_OK(api, inference->Session_ProcessRequest(session, AsHandle<flRequest>(&request), &response_handle));
  EXPECT_EQ(response_handle, AsHandle<flResponse>(&response));
  ASSERT_EQ(response.items.size(), 1u);
  EXPECT_EQ(static_cast<const fl::TextItem&>(*response.items.front()).text, "local input:hosted");
  EXPECT_EQ(request.items.size(), 1u);

  fl::KeyValuePairs options;
  options.Add("max_output_tokens", "13");
  ASSERT_FL_OK(api, inference->Session_SetOptions(session, AsHandle<flKeyValuePairs>(&options)));
  EXPECT_EQ(calls.option, "13");
  int context = 0;
  const auto callback = +[](flStreamingCallbackData, void*) { return 0; };
  ASSERT_FL_OK(api, inference->Session_SetStreamingCallback(session, callback, &context));
  EXPECT_EQ(calls.callback, callback);
  EXPECT_EQ(calls.callback_context, &context);

  flToolDefinition tool{2, "echo", "Echo the input", R"({"type":"object"})", FOUNDRY_LOCAL_TOOL_KIND_FUNCTION};
  ASSERT_FL_OK(api, inference->Session_AddToolDefinition(session, &tool));
  EXPECT_EQ(calls.tool, "echo");
  bool removed = false;
  ASSERT_FL_OK(api, inference->Session_RemoveToolDefinition(session, "echo", &removed));
  EXPECT_TRUE(removed);
  EXPECT_TRUE(calls.tool.empty());
  EXPECT_EQ(inference->Session_GetTurnCount(session), 2u);

  fl::test::StatusGuard failure{inference->Session_UndoTurns(session, 1), api};
  ASSERT_NE(failure.s, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(failure.s), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  owner.reset();
  EXPECT_EQ(calls.released_sessions, 1);
}

TEST(ApiBackendTest, VersionTwoPreflightPayloadDispatchesThroughBackendAfterSessionRelease) {
  const auto* api = FoundryLocalGetApi(2);
  ASSERT_NE(api, nullptr);
  const auto* inference = api->GetInferenceApi();
  Calls calls;
  RecordingModel model(calls);
  flSession* session = nullptr;
  ASSERT_FL_OK(api, inference->Session_Create(AsHandle<flModel>(&model), &session));
  fl::Request request;
  flRequestPreflight* preflight = nullptr;
  ASSERT_FL_OK(api, inference->Session_CreateRequestPreflight(session, AsHandle<flRequest>(&request), &preflight));
  std::unique_ptr<flRequestPreflight, decltype(inference->RequestPreflight_Release)> owner(
      preflight, inference->RequestPreflight_Release);
  inference->Session_Release(session);

  flRequestPreflightResult result{};
  result.version = FOUNDRY_LOCAL_REQUEST_PREFLIGHT_RESULT_VERSION + 1;
  fl::test::StatusGuard failure{inference->RequestPreflight_Execute(preflight, &result), api};
  ASSERT_NE(failure.s, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(failure.s), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(calls.preflights, 0);

  result.version = 2;
  ASSERT_FL_OK(api, inference->RequestPreflight_Execute(preflight, &result));
  EXPECT_EQ(calls.preflights, 1);
  EXPECT_EQ(result.version, 2u);
  EXPECT_EQ(result.prompt_tokens, 7);
  EXPECT_EQ(result.required_tokens, 10);
  EXPECT_TRUE(result.fits);
}

TEST(HostObjectsTest, RejectsOtherConnectionsIncarnationsAndObjectKinds) {
  Calls calls;
  auto manager = std::make_shared<RecordingManager>(calls);
  fl::hosting::HostObjects first("host-one", 1, manager);
  fl::hosting::HostObjects other_connection("host-one", 2, manager);
  fl::hosting::HostObjects restarted("host-two", 1, manager);
  const auto session = first.AddSession(std::make_unique<RecordingSession>(calls, fl::api::BackendKind::Embedded));
  const auto other =
      other_connection.AddSession(std::make_unique<RecordingSession>(calls, fl::api::BackendKind::Embedded));
  const auto after_restart =
      restarted.AddSession(std::make_unique<RecordingSession>(calls, fl::api::BackendKind::Embedded));
  EXPECT_EQ(session.object, other.object);
  EXPECT_EQ(session.object, after_restart.object);
  EXPECT_THROW(first.Session(other), fl::Exception);
  EXPECT_THROW(first.Session(after_restart), fl::Exception);
  EXPECT_THROW(first.Preflight(session), fl::Exception);
  EXPECT_THROW(first.AddSession(std::make_unique<RecordingSession>(calls)), fl::Exception);

  const auto decoded = fl::hosting::DecodeObjectIdentity(fl::hosting::EncodeObjectIdentity(session));
  EXPECT_EQ(first.Session(decoded)->implementation->TurnCount(), 2);
  auto malformed = fl::hosting::EncodeObjectIdentity(session);
  malformed["connection"] = -1;
  EXPECT_THROW(fl::hosting::DecodeObjectIdentity(malformed), fl::Exception);
}

TEST(HostObjectsTest, RetainedWorkPinsTheSessionAndItsCompleteManagerGraph) {
  Calls calls;
  auto manager = std::make_shared<RecordingManager>(calls);
  const std::weak_ptr<fl::api::ManagerBackend> lifetime = manager;
  auto objects = std::make_unique<fl::hosting::HostObjects>("host", 1, manager);
  const auto identity = objects->AddSession(std::make_unique<RecordingSession>(calls, fl::api::BackendKind::Embedded));
  auto admitted = objects->Session(identity);
  objects->ReleaseSession(identity);
  EXPECT_THROW(objects->Session(identity), fl::Exception);
  objects.reset();
  manager.reset();
  EXPECT_FALSE(lifetime.expired());
  EXPECT_EQ(calls.released_sessions, 0);
  EXPECT_EQ(admitted->implementation->TurnCount(), 2);
  admitted.reset();
  EXPECT_TRUE(lifetime.expired());
  EXPECT_EQ(calls.released_sessions, 1);
  EXPECT_EQ(calls.released_managers, 1);
}

TEST(HostObjectsTest, CloseRetiresLookupWhileCapturedPreflightAndSessionWorkRemainAlive) {
  Calls calls;
  auto manager = std::make_shared<RecordingManager>(calls);
  fl::hosting::HostObjects objects("host", 1, manager);
  const auto session_id = objects.AddSession(std::make_unique<RecordingSession>(calls, fl::api::BackendKind::Embedded));
  const auto preflight_id =
      objects.AddPreflight(std::make_unique<RecordingPreflight>(calls, fl::api::BackendKind::Embedded));
  auto session = objects.Session(session_id);
  auto preflight = objects.Preflight(preflight_id);
  objects.Close();
  objects.Close();
  EXPECT_THROW(objects.Session(session_id), fl::Exception);
  EXPECT_THROW(objects.Preflight(preflight_id), fl::Exception);
  EXPECT_THROW(objects.AddSession(std::make_unique<RecordingSession>(calls, fl::api::BackendKind::Embedded)),
               fl::Exception);
  EXPECT_EQ(session->implementation->TurnCount(), 2);
  EXPECT_EQ(preflight->implementation->Execute().required_tokens, 10);
  EXPECT_EQ(calls.released_preflights, 0);
  preflight.reset();
  EXPECT_EQ(calls.released_preflights, 1);
}

TEST(HostObjectsTest, ReleasesRetiredNativeObjectsOutsideTheRegistryLock) {
  Calls calls;
  auto manager = std::make_shared<RecordingManager>(calls);
  fl::hosting::HostObjects objects("host", 1, manager);
  const auto identity = objects.AddSession(std::make_unique<RecordingSession>(calls, fl::api::BackendKind::Embedded));
  calls.on_session_release = [&] { EXPECT_THROW(objects.Session(identity), fl::Exception); };
  objects.ReleaseSession(identity);
  EXPECT_EQ(calls.released_sessions, 1);
}

TEST(HostObjectsTest, RetirementCancelsActiveAndLateAdmissionsOutsideLocks) {
  Calls calls;
  auto manager = std::make_shared<RecordingManager>(calls);
  fl::hosting::HostObjects objects("host", 1, manager);
  const auto identity = objects.AddSession(std::make_unique<RecordingSession>(calls, fl::api::BackendKind::Embedded));
  auto admitted = objects.Session(identity);
  int canceled = 0;
  admitted->AdmitInvocation(1, [&] {
    ++canceled;
    EXPECT_THROW(objects.Session(identity), fl::Exception);
  });
  admitted->AdmitInvocation(2, [&] { ++canceled; });
  admitted->AdmitInvocation(3, [&] { ADD_FAILURE() << "completed invocation was cancelled"; });
  EXPECT_TRUE(admitted->HasInvocation(3));
  admitted->CompleteInvocation(3);
  EXPECT_FALSE(admitted->HasInvocation(3));
  objects.ReleaseSession(identity);
  EXPECT_TRUE(admitted->IsRetired());
  EXPECT_EQ(canceled, 2);
  admitted->Retire();
  EXPECT_EQ(canceled, 2);
  admitted->AdmitInvocation(4, [&] { ++canceled; });
  EXPECT_EQ(canceled, 3);
}

}  // namespace
