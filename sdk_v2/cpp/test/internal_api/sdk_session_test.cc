// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "api/backend.h"
#include "api/hosted_backend.h"
#include "api/request_admission.h"
#include "c_api_types.h"
#include "catalog.h"
#include "catalog/local_model_catalog.h"
#include "configuration.h"
#include "download/download_manager.h"
#include "exception.h"
#include "ep_detection/ep_types.h"
#include "hosting/host_objects.h"
#include "hosting/manager_dispatcher.h"
#include "hosting/model_dispatcher.h"
#include "hosting/process_watch.h"
#include "hosting/sdk_values.h"
#include "hosting/session_dispatcher.h"
#include "inferencing/generative/chat/search_options.h"
#include "inferencing/session/session.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "model_info.h"
#include "manager.h"
#include "util/scope_guard.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace fl::hosting {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

using LocalCallback = std::function<int(flStreamingCallbackData)>;

int InvokeLocal(flStreamingCallbackData data, void* context) { return (*static_cast<LocalCallback*>(context))(data); }

void CollectLocal(flStreamingCallbackData data, std::string& text) {
  while (auto item = AsImpl(data.item_queue)->TryPop()) {
    if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
      text += static_cast<TextItem&>(*item).text;
    }
  }
}

class ProviderBackend final : public api::ManagerBackend {
 public:
  ProviderBackend() : ManagerBackend(api::BackendKind::Embedded) {}

  api::CatalogBackend& GetCatalog(CatalogType) override {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED, "provider fixture has no catalog");
  }
  std::unique_ptr<api::ModelBackend> CaptureModel(const api::ModelBackend&) override {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED, "provider fixture has no model");
  }
  bool IsModelArtifactLoaded(const std::string&, const std::string&) const override { return false; }
  void UnloadModelArtifact(const std::string&, const std::string&) override {}
  void StartWebService() override {}
  const std::vector<std::string>& GetWebServiceUrls() const override { return urls_; }
  void StopWebService() override {}
  std::span<const flEpInfo> GetDiscoverableEps() const override { return eps_; }
  api::DeviceProviders GetRuntimeDevices() const override {
    return {{"CPU", {"CPUExecutionProvider"}}, {"GPU", {"wire-ep"}}};
  }
  void SetCatalogRuntimeDevices(api::DeviceProviders) override {}
  EpDownloadResult DownloadAndRegisterEps(const std::vector<std::string>* names,
                                          const api::EpProgressCallback& progress) override {
    if (!names || *names != std::vector<std::string>{"wire-ep"}) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "provider fixture requires wire-ep");
    }

    for (const auto percent : {12.5f, 100.0f}) {
      if (progress && !progress("wire-ep", percent)) {
        return {false, true, "cancelled", {}, {}};
      }
    }

    eps_[0].is_registered = true;
    return {true, false, "registered", {"wire-ep"}, {}};
  }
  bool IsEpDownloadInProgress() const override { return reporting_download.load(); }
  void Shutdown() override {}
  bool IsShutdownRequested() const override { return false; }

  std::atomic<bool> reporting_download{false};

 private:
  std::vector<std::string> urls_;
  std::vector<flEpInfo> eps_{{FOUNDRY_LOCAL_DATA_VERSION, "wire-ep", false}};
};

class QueueSession final : public api::SessionBackend {
 public:
  QueueSession() : SessionBackend(api::BackendKind::Embedded) {}

  void SetStreamingCallback(flStreamingCallback, void*) override {}
  void SetSessionOptions(const KeyValuePairs&) override {}
  void AddToolDefinition(ToolDefinition) override {}
  bool RemoveToolDefinition(const std::string&) override { return false; }
  size_t TurnCount() const override { return 0; }
  size_t UndoTurns(size_t) override { FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "queue session has no turns"); }
  std::unique_ptr<api::PreflightBackend> CreateRequestPreflight(const Request&) const override {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "queue session has no preflight");
  }

  size_t ProcessRequest(const Request& request, Response& response, std::optional<StreamingCallback> callback) override {
    if (!request.TryBegin()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "queue request is already running");
    }

    ScopeGuard completed([&]() noexcept { request.PublishCompletion(); });
    auto& input = static_cast<ItemQueue&>(*request.items.at(0));
    std::string text;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (true) {
      if (request.IsCancellationRequested()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "queue request cancelled");
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "queue session deadline exceeded");
      }

      if (auto item = input.WaitAndPop(5ms)) {
        const auto& part = static_cast<TextItem&>(*item).text;
        text += part;
        if (callback && callback->function) {
          ItemQueue output;
          output.Push(std::make_unique<TextItem>(part));
          if (callback->function({FOUNDRY_LOCAL_DATA_VERSION, output.AsApiType()}, callback->user_data) != 0) {
            request.Cancel(Request::CancellationReason::StreamingCallback);
          }
        }
      } else if (input.IsFinished() && input.Size() == 0) {
        break;
      }
    }

    if (!request.TryComplete()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "queue request cancelled");
    }

    response.items.push_back(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, text));
    return 0;
  }
};

class SnapshotOnlySession final : public api::SessionBackend {
 public:
  explicit SnapshotOnlySession(std::unique_ptr<api::SessionBackend> session)
      : SessionBackend(api::BackendKind::Embedded), session_(std::move(session)) {}

  void SetStreamingCallback(flStreamingCallback callback, void* context) override {
    session_->SetStreamingCallback(callback, context);
  }
  void SetSessionOptions(const KeyValuePairs& options) override { session_->SetSessionOptions(options); }
  size_t ProcessRequest(const Request& request, Response& response, std::optional<StreamingCallback> callback) override {
    return session_->ProcessRequest(request, response, std::move(callback));
  }
  void AddToolDefinition(ToolDefinition definition) override { session_->AddToolDefinition(std::move(definition)); }
  bool RemoveToolDefinition(const std::string& name) override { return session_->RemoveToolDefinition(name); }
  size_t TurnCount() const override {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "turn count must be captured inside the native operation");
  }
  size_t UndoTurns(size_t count) override { return session_->UndoTurns(count); }
  std::unique_ptr<api::PreflightBackend> CreateRequestPreflight(const Request& request) const override {
    return session_->CreateRequestPreflight(request);
  }

 private:
  std::unique_ptr<api::SessionBackend> session_;
};

class SdkSessionTest : public testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<uint64_t> sequence{0};
    const auto pid = std::to_string(CurrentProcessIdentity().pid);
    const auto scratch = std::filesystem::path(FOUNDRY_LOCAL_TEST_SCRATCH_DIR);
    ASSERT_TRUE(scratch.is_absolute());
    std::filesystem::create_directories(scratch);
    ASSERT_FALSE(std::filesystem::is_symlink(scratch));
    std::string suffix;
    do {
      suffix = pid + "-" + std::to_string(sequence.fetch_add(1));
      root_ = scratch / ("session-" + suffix);
    } while (!std::filesystem::create_directory(root_));
    owns_root_ = true;

#ifdef _WIN32
    endpoint_ = LR"(\\.\pipe\foundry-sdk-session-)" + std::filesystem::path(suffix).native();
#else
    std::filesystem::permissions(root_, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    socket_directory_ = test::MakePrivateIpcDirectory();
    endpoint_ = socket_directory_ / "s";
#endif
    Configuration config;
    config.app_name = "sdk-session-test";
    config.app_data_dir = (root_ / "data").string();
    config.model_cache_dir = (root_ / "models").string();
    config.logs_dir = (root_ / "logs").string();
    config.disable_nonessential_telemetry = true;
    manager_ = api::CreateEmbeddedBackend(config);
    ModelInfo metadata;
    metadata.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "coding");
    auto* model =
        manager_->GetCatalog(CatalogType::kLocal)
            .RegisterModel((std::filesystem::path(FOUNDRY_LOCAL_TEST_DATA_DIR) / "tiny-paged-attention").string(),
                           "tiny-rpc:1", metadata);
    model->Load();
    objects_ = std::make_unique<HostObjects>("session-test-host", 1, manager_);
    model_dispatcher_ = std::make_unique<ModelDispatcher>(manager_);
    lifetime_ = std::make_unique<HostLifetime>(std::chrono::steady_clock::now() + 10s);
    ASSERT_TRUE(lifetime_->AddOwner(1, std::chrono::steady_clock::now()));
    manager_dispatcher_ = std::make_unique<ManagerDispatcher>(manager_, *lifetime_);
    identity_ = objects_->AddSession(model->CreateSession());
    Listener listener(endpoint_);
    auto channel = Connect(endpoint_);
    auto accepted = listener.Accept();
    server_ = std::make_unique<SdkConnection>(std::move(accepted.first), SdkConnection::Side::Server);
    client_ = std::make_shared<SdkConnection>(std::move(channel), SdkConnection::Side::Client);
    accept_worker_ = std::thread([this] {
      std::vector<std::thread> executions;
      while (auto call = server_->Accept()) {
        executions.emplace_back([this, call] {
          try {
            if (ModelDispatcher::Handles(call->Op())) {
              model_calls_.fetch_add(1, std::memory_order_relaxed);
              model_dispatcher_->Dispatch(call, *objects_);
            } else if (ManagerDispatcher::Handles(call->Op())) {
              manager_dispatcher_->Dispatch(call, 1);
            } else {
              SessionDispatcher(*objects_, *server_).Dispatch(call);
            }
          } catch (...) {
            server_->Close();
          }
        });
      }

      for (auto& execution : executions) {
        execution.join();
      }
    });
  }

  void TearDown() override {
    if (objects_) {
      objects_->Close();
    }
    if (client_) {
      client_->Close();
    }
    if (server_) {
      server_->Close();
    }

    for (auto& worker : callers_) {
      worker.join();
    }
    if (accept_worker_.joinable()) {
      accept_worker_.join();
    }

    client_.reset();
    server_.reset();
    objects_.reset();
    model_dispatcher_.reset();
    manager_dispatcher_.reset();
    lifetime_.reset();
    manager_.reset();
    if (!socket_directory_.empty()) {
      std::filesystem::remove_all(socket_directory_);
    }
    if (owns_root_) {
      std::filesystem::remove_all(root_);
    }
  }

  std::future<SdkResult> StartCall(Operation operation, Json payload, SdkConnection::EventCallback callback = {},
                                   uint64_t id = 0) {
    if (id == 0) {
      id = client_->AllocateInvocation();
    }

    std::packaged_task<SdkResult()> task(
        [this, id, operation, payload = std::move(payload), callback = std::move(callback)] {
          return client_->Call(id, operation, payload, callback);
        });
    auto result = task.get_future();
    callers_.emplace_back(std::move(task));
    return result;
  }

  SdkResult FinishCall(std::future<SdkResult>& result) {
    if (result.wait_for(10s) != std::future_status::ready) {
      client_->Close();
      throw std::runtime_error("SDK native session deadline exceeded");
    }

    return result.get();
  }

  SdkResult Call(Operation operation, Json payload, SdkConnection::EventCallback callback = {}) {
    auto result = StartCall(operation, std::move(payload), std::move(callback));
    return FinishCall(result);
  }

  Request CodingRequest() const {
    Request request;
    request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Remember the word sapphire."));
    request.options.Add("max_output_tokens", "17");
    request.options.Add("temperature", "0");
    return request;
  }

  Json ProcessPayload(bool streaming = false) const {
    auto request = CodingRequest();
    return {
        {"object", EncodeObjectIdentity(identity_)}, {"request", EncodeRequest(request, {})}, {"streaming", streaming}};
  }

  std::shared_ptr<api::HostedContext> ClientContext() const {
    return std::make_shared<api::HostedContext>(client_, "session-test-host", 1);
  }

  static std::string AssistantText(const Response& response) {
    for (const auto& item : response.items) {
      if (item->type == FOUNDRY_LOCAL_ITEM_MESSAGE) {
        const auto& message = static_cast<const MessageItem&>(*item);
        if (message.role == FOUNDRY_LOCAL_ROLE_ASSISTANT) {
          return message.GetSimpleText();
        }
      }
    }

    return {};
  }

  std::shared_ptr<api::ManagerBackend> manager_;
  std::unique_ptr<ModelDispatcher> model_dispatcher_;
  std::unique_ptr<HostLifetime> lifetime_;
  std::unique_ptr<ManagerDispatcher> manager_dispatcher_;
  std::unique_ptr<HostObjects> objects_;
  RemoteObjectIdentity identity_;
  std::shared_ptr<SdkConnection> client_;
  std::unique_ptr<SdkConnection> server_;
  std::filesystem::path endpoint_;
  std::filesystem::path socket_directory_;
  std::filesystem::path root_;
  bool owns_root_ = false;
  std::thread accept_worker_;
  std::vector<std::thread> callers_;
  std::atomic<size_t> model_calls_{0};
};

TEST_F(SdkSessionTest, DormantHostedManagerKeepsMetadataDownloadsAndRegistrationLocal) {
  int observations = 0;
  auto hosted = api::CreateHostedBackend(manager_, [&](bool acquire) -> std::shared_ptr<api::HostedContext> {
    if (acquire) {
      throw std::runtime_error("metadata must not start a host");
    }

    ++observations;
    return {};
  });
  auto& catalog = hosted->GetCatalog(CatalogType::kLocal);
  auto* model = catalog.GetModelVariant("tiny-rpc:1");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->Kind(), api::BackendKind::Hosted);
  EXPECT_EQ(model->Info().task, "coding");
  EXPECT_FALSE(model->Variants().empty());
  int progress = 0;
  model->Download([&](float percent) {
    EXPECT_EQ(percent, 100);
    ++progress;
    return 0;
  });
  EXPECT_EQ(progress, 1);
  EXPECT_EQ(observations, 0);
  // The embedded test fixture has weights loaded; hosted readiness must not consult that local execution state.
  EXPECT_TRUE(manager_->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1")->IsLoaded());
  EXPECT_FALSE(model->IsLoaded());
  EXPECT_TRUE(catalog.GetLoadedModels().empty());
  EXPECT_TRUE(hosted->GetWebServiceUrls().empty());
  EXPECT_FALSE(hosted->IsEpDownloadInProgress());
  model->Unload();
  ModelInfo metadata;
  metadata.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "coding");
  auto* registered = catalog.RegisterModel(model->GetPath(), "dormant-rpc:1", metadata);
  ASSERT_NE(registered, nullptr);
  EXPECT_EQ(registered->Kind(), api::BackendKind::Hosted);
  catalog.UnregisterModel("dormant-rpc:1");
  EXPECT_GE(observations, 6);
  EXPECT_EQ(model_calls_.load(), 0);
  hosted->Shutdown();
  EXPECT_FALSE(manager_->IsShutdownRequested());
}

TEST_F(SdkSessionTest, HostedCacheRemovalAcquiresTheHostEvenWhenItWasPreviouslyAbsent) {
  int acquired = 0;
  auto hosted = api::CreateHostedBackend(manager_, [&](bool acquire) -> std::shared_ptr<api::HostedContext> {
    if (!acquire) {
      ADD_FAILURE() << "An absence observation cannot protect a subsequent local cache deletion";
      return {};
    }

    ++acquired;
    return ClientContext();
  });
  auto* model = hosted->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1");
  ASSERT_NE(model, nullptr);
  const auto before = model_calls_.load();
  // This registration cannot be deleted, but the authoritative host must make that decision.
  EXPECT_THROW(model->RemoveFromCache(), Exception);
  EXPECT_EQ(acquired, 1);
  EXPECT_EQ(model_calls_.load(), before + 1);
  EXPECT_TRUE(std::filesystem::exists(model->GetPath()));
}

TEST_F(SdkSessionTest, HostedManagerRoutesNormalModelAndSessionOperationsOverItsSingleLazyOwner) {
  auto context = ClientContext();
  int acquired = 0;
  int observed = 0;
  auto hosted = api::CreateHostedBackend(manager_, [&](bool acquire) {
    acquire ? ++acquired : ++observed;
    return context;
  });
  auto& catalog = hosted->GetCatalog(CatalogType::kLocal);
  auto* model = catalog.GetModelVariant("tiny-rpc:1");
  ASSERT_NE(model, nullptr);
  EXPECT_TRUE(model->IsLoaded());
  EXPECT_EQ(acquired, 0);
  EXPECT_EQ(observed, 1);
  EXPECT_EQ(catalog.GetLoadedModels().size(), 1);
  const auto io = model->GetInputOutputInfo();
  EXPECT_GT(io.num_inputs, 0);
  EXPECT_GT(io.num_outputs, 0);
  EXPECT_EQ(io.outputs[0]->type, FOUNDRY_LOCAL_ITEM_MESSAGE);
  ASSERT_GT(io.num_outputs, 1);
  ASSERT_EQ(io.outputs[1]->type, FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(static_cast<const TextItem&>(*io.outputs[1]).text_type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON);
  auto* embedded = manager_->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1");
  auto* container = catalog.GetModel(model->Info().alias);
  ASSERT_NE(container, nullptr);
  EXPECT_THROW(container->SelectVariant(*embedded), Exception);
  container->SelectVariant(*model);
  objects_->ReleaseSession(identity_);
  model->Unload();
  EXPECT_FALSE(model->IsLoaded());
  const auto before_load = model_calls_.load();
  model->Load();
  EXPECT_EQ(model_calls_.load(), before_load + 1);
  auto session = model->CreateSession();
  EXPECT_EQ(acquired, 1);
  EXPECT_THROW(model->Unload(), Exception);
  auto request = CodingRequest();
  auto preflight = session->CreateRequestPreflight(request);
  EXPECT_TRUE(preflight->Execute().fits);
  preflight.reset();
  Response response;
  session->ProcessRequest(request, response);
  EXPECT_EQ(AssistantText(response).size(), 17);
  EXPECT_EQ(session->TurnCount(), 1);
  session->UndoTurns(1);
  EXPECT_EQ(session->TurnCount(), 0);
  session.reset();
  model->Unload();
  EXPECT_FALSE(model->IsLoaded());
  EXPECT_EQ(acquired, 1);
}

TEST_F(SdkSessionTest, HostedManagerReconcilesLiveRegistrationsAndRetainsCapturedMetadata) {
  auto context = ClientContext();
  auto hosted = api::CreateHostedBackend(manager_, [&](bool) { return context; });
  auto& catalog = hosted->GetCatalog(CatalogType::kLocal);
  ModelInfo metadata;
  metadata.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "coding");
  metadata.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT, 1);
  const auto path = (std::filesystem::path(FOUNDRY_LOCAL_TEST_DATA_DIR) / "tiny-paged-attention").string();
  auto* model = catalog.RegisterModel(path, "hosted-live:1", metadata);
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model_calls_.load(), 1);
  auto capture = hosted->CaptureModel(*model);
  EXPECT_EQ(capture->Info().model_id, "hosted-live:1");
  EXPECT_EQ(capture->Info().task, "coding");
  EXPECT_EQ(capture->Variants().front(), capture.get());
  model->Load();
  EXPECT_THROW(catalog.UnregisterModel("hosted-live:1"), Exception);
  model->Unload();
  catalog.UnregisterModel("hosted-live:1");
  EXPECT_THROW(model->Load(), Exception);
  EXPECT_FALSE(model->IsLoaded());
  hosted.reset();
  EXPECT_EQ(capture->Info().model_id, "hosted-live:1");
  EXPECT_THROW(capture->CreateSession(), Exception);
}

TEST_F(SdkSessionTest, HostedManagerDisposalClosesOnlyTheClientAndInvalidatesItsChildren) {
  auto context = ClientContext();
  auto hosted = api::CreateHostedBackend(manager_, [&](bool) { return context; });
  auto* model = hosted->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1");
  auto session = model->CreateSession();
  auto request = CodingRequest();
  auto preflight = session->CreateRequestPreflight(request);
  hosted->Shutdown();
  hosted->Shutdown();
  EXPECT_TRUE(hosted->IsShutdownRequested());
  EXPECT_FALSE(manager_->IsShutdownRequested());
  EXPECT_TRUE(manager_->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1")->IsLoaded());
  EXPECT_THROW(model->Load(), Exception);
  EXPECT_THROW(preflight->Execute(), Exception);
  Response response;
  EXPECT_THROW(session->ProcessRequest(request, response), Exception);
  auto independent = manager_->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1")->CreateSession();
  independent->ProcessRequest(request, response);
  EXPECT_EQ(AssistantText(response).size(), 17);
}

TEST_F(SdkSessionTest, HostedManagerNeverReconnectsOrFallsBackAfterLosingItsOwner) {
  auto context = ClientContext();
  int connections = 0;
  auto hosted = api::CreateHostedBackend(manager_, [&](bool) {
    ++connections;
    return context;
  });
  auto* model = hosted->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1");
  model->Load();
  EXPECT_EQ(connections, 1);
  server_->Close();
  EXPECT_THROW(model->Load(), std::exception);
  EXPECT_THROW(model->IsLoaded(), std::exception);
  EXPECT_THROW(model->CreateSession(), std::exception);
  EXPECT_EQ(connections, 1);
  EXPECT_TRUE(manager_->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1")->IsLoaded());
}

TEST_F(SdkSessionTest, HostedReadinessPreservesFailedObservationInsteadOfReportingAbsence) {
  auto hosted = api::CreateHostedBackend(manager_, [&](bool) -> std::shared_ptr<api::HostedContext> {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "incompatible host runtime");
  });
  auto* model = hosted->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1");
  EXPECT_THROW(model->IsLoaded(), Exception);
  EXPECT_THROW(hosted->GetCatalog(CatalogType::kLocal).GetLoadedModels(), Exception);
  EXPECT_THROW(model->Load(), Exception);
  EXPECT_FALSE(hosted->IsShutdownRequested());
}

TEST_F(SdkSessionTest, HostedManagerUsesHostWebServiceStateAndPreservesBusyStop) {
  auto context = ClientContext();
  int acquired = 0;
  auto hosted = api::CreateHostedBackend(manager_, [&](bool acquire) {
    acquired += acquire;
    return context;
  });
  EXPECT_TRUE(hosted->GetWebServiceUrls().empty());
  EXPECT_EQ(acquired, 0);
  hosted->StartWebService();
  ASSERT_EQ(acquired, 1);
  ASSERT_FALSE(hosted->GetWebServiceUrls().empty());
  const auto& original_urls = hosted->GetWebServiceUrls();
  const auto* original_storage = original_urls.data();
  const auto* original_text = original_urls.front().c_str();
  EXPECT_EQ(hosted->GetWebServiceUrls().data(), original_storage);
  EXPECT_EQ(hosted->GetWebServiceUrls().front().c_str(), original_text);
  ASSERT_TRUE(lifetime_->AddOwner(2, std::chrono::steady_clock::now()));
  EXPECT_THROW(hosted->StopWebService(), Exception);
  EXPECT_FALSE(hosted->GetWebServiceUrls().empty());
  ASSERT_TRUE(lifetime_->RemoveOwner(2, std::chrono::steady_clock::now()));
  auto http = lifetime_->TryAdmitHttpWork(std::chrono::steady_clock::now());
  ASSERT_TRUE(http);
  EXPECT_THROW(hosted->StopWebService(), Exception);
  http.Complete(std::chrono::steady_clock::now());
  hosted->StopWebService();
  EXPECT_TRUE(hosted->GetWebServiceUrls().empty());
  EXPECT_EQ(original_urls.data(), original_storage);
  EXPECT_EQ(original_urls.front().c_str(), original_text);
  EXPECT_FALSE(hosted->IsShutdownRequested());
  EXPECT_FALSE(manager_->IsShutdownRequested());
  EXPECT_TRUE(hosted->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1")->IsLoaded());
}

TEST_F(SdkSessionTest, HostedProviderDiscoveryDoesNotAcquireAndRegistrationUsesTheLazyOwner) {
  auto context = ClientContext();
  int acquired = 0;
  auto hosted = api::CreateHostedBackend(manager_, [&](bool acquire) {
    acquired += acquire;
    return context;
  });
  const auto discoverable = hosted->GetDiscoverableEps();
  const auto native = manager_->GetDiscoverableEps();
  ASSERT_EQ(discoverable.size(), native.size());
  for (size_t i = 0; i < native.size(); ++i) {
    EXPECT_STREQ(discoverable[i].name, native[i].name);
    EXPECT_EQ(discoverable[i].is_registered, native[i].is_registered);
  }

  EXPECT_EQ(acquired, 0);
  const std::vector<std::string> select_none;
  const auto result = hosted->DownloadAndRegisterEps(&select_none, {});
  EXPECT_TRUE(result.success);
  EXPECT_FALSE(result.cancelled);
  EXPECT_TRUE(result.registered_eps.empty());
  EXPECT_EQ(acquired, 1);
  EXPECT_FALSE(hosted->IsEpDownloadInProgress());
}

TEST_F(SdkSessionTest, HostedProviderProgressPreservesCancellationAndExceptionsWithoutLosingItsOwner) {
  manager_dispatcher_ = std::make_unique<ManagerDispatcher>(std::make_shared<ProviderBackend>(), *lifetime_);
  auto context = ClientContext();
  int acquired = 0;
  auto hosted = api::CreateHostedBackend(manager_, [&](bool acquire) {
    acquired += acquire;
    return context;
  });
  const std::vector<std::string> names{"wire-ep"};
  const auto discoverable = hosted->GetDiscoverableEps();
  ASSERT_EQ(discoverable.size(), 1);
  const auto* stable_struct = discoverable.data();
  const auto* stable_name = discoverable[0].name;
  EXPECT_EQ(acquired, 0);
  std::vector<float> progress;
  const auto success = hosted->DownloadAndRegisterEps(&names, [&](const std::string& name, float percent) {
    EXPECT_EQ(name, "wire-ep");
    EXPECT_TRUE(hosted->IsEpDownloadInProgress());
    progress.push_back(percent);
    return true;
  });
  EXPECT_TRUE(success.success);
  EXPECT_EQ(progress, (std::vector<float>{12.5f, 100.0f}));
  EXPECT_EQ(success.registered_eps, names);
  EXPECT_TRUE(stable_struct[0].is_registered);
  const auto cancelled = hosted->DownloadAndRegisterEps(&names, [](const auto&, auto) { return false; });
  EXPECT_TRUE(cancelled.cancelled);
  EXPECT_FALSE(hosted->IsEpDownloadInProgress());
  EXPECT_THROW(hosted->DownloadAndRegisterEps(
                   &names, [](const auto&, auto) -> bool { throw std::runtime_error("EP callback"); }),
               std::runtime_error);
  EXPECT_FALSE(hosted->IsEpDownloadInProgress());
  EXPECT_TRUE(hosted->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1")->IsLoaded());
  EXPECT_EQ(acquired, 1);
  EXPECT_EQ(hosted->GetDiscoverableEps().data(), stable_struct);
  EXPECT_STREQ(stable_name, "wire-ep");
}

TEST_F(SdkSessionTest, HostedProviderCallbackCanDisposeItsManagerWithoutDestroyingAdmittedState) {
  manager_dispatcher_ = std::make_unique<ManagerDispatcher>(std::make_shared<ProviderBackend>(), *lifetime_);
  auto context = ClientContext();
  auto hosted = api::CreateHostedBackend(manager_, [&](bool) { return context; });
  const std::vector<std::string> names{"wire-ep"};
  EXPECT_THROW(hosted->DownloadAndRegisterEps(&names,
                                              [&](const auto&, auto) {
                                                hosted.reset();
                                                return true;
                                              }),
               std::exception);
  EXPECT_EQ(hosted, nullptr);
  EXPECT_FALSE(manager_->IsShutdownRequested());
}

TEST_F(SdkSessionTest, HostedRuntimeDeviceObservationDoesNotAcquireOrRegisterProvidersInTheClient) {
  auto provider = std::make_shared<ProviderBackend>();
  manager_dispatcher_ = std::make_unique<ManagerDispatcher>(provider, *lifetime_);
  auto context = ClientContext();
  int acquired = 0;
  auto hosted = api::CreateHostedBackend(manager_, [&](bool acquire) {
    acquired += acquire;
    return context;
  });
  const auto local_devices = manager_->GetRuntimeDevices();
  const auto remote_devices = hosted->GetRuntimeDevices();
  EXPECT_EQ(remote_devices.at("GPU"), (std::vector<std::string>{"wire-ep"}));
  hosted->SetCatalogRuntimeDevices(remote_devices);
  EXPECT_EQ(manager_->GetRuntimeDevices(), local_devices);
  EXPECT_EQ(acquired, 0);
  provider->reporting_download = true;
  EXPECT_TRUE(hosted->IsEpDownloadInProgress());
  provider->reporting_download = false;
  EXPECT_FALSE(hosted->IsEpDownloadInProgress());
  EXPECT_EQ(acquired, 0);
  context->Close();
  EXPECT_FALSE(hosted->IsEpDownloadInProgress());
  hosted->Shutdown();
  EXPECT_FALSE(hosted->IsEpDownloadInProgress());
}

TEST_F(SdkSessionTest, HostedSessionAndCapturedPreflightKeepTheClientDependencyGraphAlive) {
  auto owner = std::make_shared<int>(0);
  const std::weak_ptr<int> observed = owner;
  auto context = ClientContext();
  auto session = api::CreateHostedSession(context, identity_, owner);
  owner.reset();
  auto request = CodingRequest();
  auto preflight = session->CreateRequestPreflight(request);
  session.reset();
  EXPECT_FALSE(observed.expired());
  EXPECT_TRUE(preflight->Execute().fits);
  preflight.reset();
  EXPECT_TRUE(observed.expired());
}

TEST_F(SdkSessionTest, NativeArtifactMutationRechecksExpectedSelectionUnderItsLifecycleLock) {
  auto* model = manager_->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1");
  const auto replacement = (root_ / "replacement").string();
  EXPECT_THROW(model->Load(replacement), Exception);
  EXPECT_TRUE(model->IsLoaded());
  EXPECT_EQ(Call(Operation::IsLoaded, {{"model", EncodeModelSelection(CatalogType::kLocal, *model)}}).value, true);
}

TEST_F(SdkSessionTest, CatalogRefreshRetiresMetadataChangedAtTheSameIdAndPathBetweenPeerObservations) {
  auto& catalog = manager_->GetCatalog(CatalogType::kLocal);
  ModelInfo original;
  original.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "coding");
  original.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT, 0);
  const auto path = (std::filesystem::path(FOUNDRY_LOCAL_TEST_DATA_DIR) / "tiny-paged-attention").string();
  catalog.RegisterModel(path, "peer-metadata:1", original);
  auto* old = catalog.GetModelVariant("peer-metadata:1");
  ASSERT_NE(old, nullptr);
  // Keep the first catalog idle while a peer removes and replaces this registration in their shared persistent store.
  auto& core = Manager::Instance();
  LocalModelCatalog peer(
      core.GetDownloadManager().GetCacheDirectory(),
      [&](ModelInfo info, std::string local_path) {
        return Model::FromLocalRegistration(std::move(info), std::move(local_path), core.GetDownloadManager(),
                                            core.GetModelLoadManager());
      },
      core.GetLogger());
  peer.UnregisterModel("peer-metadata:1");
  auto replacement = original;
  replacement.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT, 1);
  peer.RegisterModel(path, "peer-metadata:1", replacement);
  auto* current = catalog.GetModelVariant("peer-metadata:1");
  ASSERT_NE(current, nullptr);
  EXPECT_NE(current, old);
  EXPECT_EQ(*current->Info().GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT), 1);
  EXPECT_EQ(*old->Info().GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT), 0);
  EXPECT_THROW(old->Load(), Exception);
  current->Load();
  EXPECT_TRUE(current->IsLoaded());
}

TEST_F(SdkSessionTest, ExecutesNativeCodingSessionAndReturnsOwnedResponseAndUsage) {
  const auto result = Call(Operation::ProcessRequest, ProcessPayload());
  ASSERT_EQ(result.status, 0) << result.error;
  const auto response = DecodeResponse(result.value.at("response"), {});
  EXPECT_EQ(AssistantText(response).size(), 17);
  EXPECT_EQ(response.usage.completion_tokens, 17);
  EXPECT_GT(response.usage.prompt_tokens, 0);
  EXPECT_EQ(response.usage.total_tokens, response.usage.prompt_tokens + 17);
  EXPECT_EQ(result.value.at("turns"), 1);

  const auto undo = Call(Operation::UndoTurns, {{"object", EncodeObjectIdentity(identity_)}, {"count", uint64_t{0}}});
  EXPECT_EQ(undo.status, 0) << undo.error;
  EXPECT_EQ(undo.value, 1);
}

TEST_F(SdkSessionTest, AcknowledgesOrderedStreamingBeforeTheNativeTurnCommits) {
  std::string streamed;
  uint64_t last_sequence = 0;
  const auto result = Call(Operation::ProcessRequest, ProcessPayload(true), [&](auto id, const Json& event) {
    const auto sequence = event.at("sequence").get<uint64_t>();
    EXPECT_EQ(sequence, ++last_sequence);
    for (const auto& encoded : event.at("items")) {
      auto item = DecodeItem(encoded, {});
      if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
        streamed += static_cast<const TextItem&>(*item).text;
      }
    }

    client_->PushInput(id, {{"kind", "callback"}, {"sequence", sequence}, {"cancel", false}, {"exception", nullptr}});
  });
  ASSERT_EQ(result.status, 0) << result.error;
  EXPECT_GT(last_sequence, 0);
  EXPECT_EQ(streamed, AssistantText(DecodeResponse(result.value.at("response"), {})));
}

TEST_F(SdkSessionTest, RepliesUseTurnCountsCapturedByTheNativeOperationWithoutRereadingSessionState) {
  auto session = objects_->Session(identity_);
  session->implementation = std::make_unique<SnapshotOnlySession>(std::move(session->implementation));
  const auto result = Call(Operation::ProcessRequest, ProcessPayload());
  ASSERT_EQ(result.status, 0) << result.error;
  EXPECT_EQ(result.value.at("turns"), 1);

  const auto undo = Call(Operation::UndoTurns, {{"object", EncodeObjectIdentity(identity_)}, {"count", uint64_t{1}}});
  ASSERT_EQ(undo.status, 0) << undo.error;
  EXPECT_EQ(undo.value, 0);
}

TEST_F(SdkSessionTest, CallbackCancellationRollsBackTheTurnAndLeavesTheSessionReusable) {
  int callbacks = 0;
  const auto canceled = Call(Operation::ProcessRequest, ProcessPayload(true), [&](auto id, const Json& event) {
    ++callbacks;
    client_->PushInput(
        id, {{"kind", "callback"}, {"sequence", event.at("sequence")}, {"cancel", true}, {"exception", nullptr}});
  });
  EXPECT_EQ(canceled.status, FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED) << canceled.error;
  EXPECT_GT(callbacks, 0);
  EXPECT_EQ(objects_->Session(identity_)->implementation->TurnCount(), 0);
  const auto next = Call(Operation::ProcessRequest, ProcessPayload());
  EXPECT_EQ(next.status, 0) << next.error;
  EXPECT_EQ(next.value.at("turns"), 1);
}

TEST_F(SdkSessionTest, CapturedPreflightSurvivesSourceSessionReleaseAndRemainsOneShot) {
  const auto source = ProcessPayload();
  const auto capture = Call(Operation::CapturePreflight,
                            {{"object", EncodeObjectIdentity(identity_)}, {"request", source.at("request")}});
  ASSERT_EQ(capture.status, 0) << capture.error;
  const auto release = Call(Operation::ReleaseSession, {{"object", EncodeObjectIdentity(identity_)}});
  ASSERT_EQ(release.status, 0) << release.error;
  const auto preflight = Call(Operation::ExecutePreflight, {{"object", capture.value}});
  ASSERT_EQ(preflight.status, 0) << preflight.error;
  const auto budget = DecodeBudget(preflight.value);
  EXPECT_GT(budget.prompt_tokens, 0);
  EXPECT_EQ(budget.output_reserve_tokens, 17);
  EXPECT_TRUE(budget.fits);
  const auto twice = Call(Operation::ExecutePreflight, {{"object", capture.value}});
  EXPECT_EQ(twice.status, FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
}

TEST_F(SdkSessionTest, OwnerRetirementUnblocksAnUnacknowledgedStreamingCallback) {
  auto admitted = objects_->Session(identity_);
  const auto canceled =
      Call(Operation::ProcessRequest, ProcessPayload(true), [&](auto, const Json&) { objects_->Close(); });
  EXPECT_EQ(canceled.status, FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED) << canceled.error;
  EXPECT_EQ(admitted->implementation->TurnCount(), 0);
  EXPECT_THROW(objects_->Session(identity_), Exception);
}

TEST_F(SdkSessionTest, PreservesCallbackExceptionStatusAndRollsBackTheTurn) {
  const auto result = Call(Operation::ProcessRequest, ProcessPayload(true), [&](auto id, const Json& event) {
    client_->PushInput(id, {{"kind", "callback"},
                            {"sequence", event.at("sequence")},
                            {"cancel", false},
                            {"exception", "callback failed"}});
  });
  EXPECT_EQ(result.status, FOUNDRY_LOCAL_ERROR_INTERNAL) << result.error;
  EXPECT_NE(result.error.find("callback failed"), std::string::npos);
  EXPECT_EQ(objects_->Session(identity_)->implementation->TurnCount(), 0);
}

TEST_F(SdkSessionTest, MalformedInputPreservesItsErrorInsteadOfTheInducedNativeCancellation) {
  const auto result = Call(Operation::ProcessRequest, ProcessPayload(true), [&](auto id, const Json& event) {
    client_->PushInput(id, {{"kind", "callback"},
                            {"sequence", event.at("sequence").get<uint64_t>() + 1},
                            {"cancel", false},
                            {"exception", nullptr}});
  });
  EXPECT_EQ(result.status, FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT) << result.error;
  EXPECT_NE(result.error.find("unexpected SDK streaming acknowledgment"), std::string::npos);
  EXPECT_EQ(objects_->Session(identity_)->implementation->TurnCount(), 0);
  EXPECT_EQ(Call(Operation::ProcessRequest, ProcessPayload()).status, 0);
}

TEST_F(SdkSessionTest, CallbackScopedMutationsFailWithoutBlockingTheStream) {
  int rejected = 0;
  const auto result = Call(Operation::ProcessRequest, ProcessPayload(true), [&](auto id, const Json& event) {
    const auto nested = client_->Call(
        Operation::SessionOptions,
        {{"object", EncodeObjectIdentity(identity_)}, {"options", Json::object()}, {"callback_parent", id}});
    EXPECT_EQ(nested.status, FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
    ++rejected;
    client_->PushInput(
        id, {{"kind", "callback"}, {"sequence", event.at("sequence")}, {"cancel", false}, {"exception", nullptr}});
  });
  EXPECT_EQ(result.status, 0) << result.error;
  EXPECT_GT(rejected, 0);
}

TEST_F(SdkSessionTest, OverlappingRequestsPreserveNativeSessionSerializationAndSeparateCallbacks) {
  std::promise<void> entered, resume;
  auto entry = entered.get_future();
  auto continuation = resume.get_future().share();
  bool first_callback = true;
  std::string streamed;
  auto first = StartCall(Operation::ProcessRequest, ProcessPayload(true), [&](auto id, const Json& event) {
    if (first_callback) {
      first_callback = false;
      entered.set_value();
      if (continuation.wait_for(5s) != std::future_status::ready) {
        throw std::runtime_error("overlapping session admission deadline exceeded");
      }
    }

    for (const auto& encoded : event.at("items")) {
      auto item = DecodeItem(encoded, {});
      if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
        streamed += static_cast<TextItem&>(*item).text;
      }
    }

    client_->PushInput(
        id, {{"kind", "callback"}, {"sequence", event.at("sequence")}, {"cancel", false}, {"exception", nullptr}});
  });
  EXPECT_EQ(entry.wait_for(2s), std::future_status::ready);
  const auto next_id = client_->AllocateInvocation();
  auto second = StartCall(Operation::ProcessRequest, ProcessPayload(), {}, next_id);
  auto session = objects_->Session(identity_);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!session->HasInvocation(next_id) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }

  const bool admitted = session->HasInvocation(next_id);
  resume.set_value();
  EXPECT_TRUE(admitted);
  const auto first_result = FinishCall(first);
  const auto second_result = FinishCall(second);
  ASSERT_EQ(first_result.status, 0) << first_result.error;
  ASSERT_EQ(second_result.status, 0) << second_result.error;
  EXPECT_EQ(streamed, AssistantText(DecodeResponse(first_result.value.at("response"), {})));
  EXPECT_EQ(first_result.value.at("turns"), 1);
  EXPECT_EQ(second_result.value.at("turns"), 2);
}

TEST_F(SdkSessionTest, HostedAdapterUsesNormalMethodsAndMaterializesLocalOwnedResults) {
  auto context = ClientContext();
  auto session = api::CreateHostedSession(context, identity_);
  KeyValuePairs options;
  options.Add("temperature", "0");
  session->SetSessionOptions(options);
  session->AddToolDefinition({"lookup", "Look up a value", R"({"type":"object","properties":{}})"});
  EXPECT_THROW(session->AddToolDefinition({"lookup", "duplicate", R"({"type":"object"})"}), Exception);
  EXPECT_TRUE(session->RemoveToolDefinition("lookup"));
  EXPECT_FALSE(session->RemoveToolDefinition("lookup"));
  auto request = CodingRequest();
  Response response;
  std::string streamed;
  LocalCallback collect = [&](auto data) {
    CollectLocal(data, streamed);
    return 0;
  };
  session->SetStreamingCallback(InvokeLocal, &collect);
  session->ProcessRequest(request, response);
  EXPECT_EQ(streamed, AssistantText(response));
  EXPECT_EQ(streamed.size(), 17);
  EXPECT_EQ(response.usage.completion_tokens, 17);
  EXPECT_TRUE(request.IsCompleted());
  EXPECT_EQ(session->TurnCount(), 1);
  session->UndoTurns(0);
  EXPECT_EQ(session->TurnCount(), 1);
  session->UndoTurns(1);
  EXPECT_EQ(session->TurnCount(), 0);
  context->Close();
  EXPECT_EQ(streamed, AssistantText(response));
  EXPECT_THROW(session->SetSessionOptions(options), Exception);
}

TEST_F(SdkSessionTest, HostedAdapterSupportsSynchronousCancelAndCallbackExceptionsWithoutLosingTheConnection) {
  auto context = ClientContext();
  auto session = api::CreateHostedSession(context, identity_);
  auto request = CodingRequest();
  Response response;
  std::string discarded;
  LocalCallback callback = [&](auto data) {
    CollectLocal(data, discarded);
    api::CancelRequest(request);
    return 0;
  };
  session->SetStreamingCallback(InvokeLocal, &callback);
  try {
    session->ProcessRequest(request, response);
    FAIL() << "expected callback-initiated cancellation";
  } catch (const Exception& failure) {
    EXPECT_EQ(failure.code(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED) << failure.what();
  }

  EXPECT_TRUE(request.IsCompleted());
  EXPECT_EQ(session->TurnCount(), 0);
  callback = [&](auto data) {
    CollectLocal(data, discarded);
    return 1;
  };
  try {
    session->ProcessRequest(request, response);
    FAIL() << "expected callback return to cancel the request";
  } catch (const Exception& failure) {
    EXPECT_EQ(failure.code(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED) << failure.what();
  }

  EXPECT_TRUE(request.IsCompleted());
  EXPECT_EQ(session->TurnCount(), 0);
  callback = [](auto) -> int { throw std::runtime_error("local callback exploded"); };
  try {
    session->ProcessRequest(request, response);
    FAIL() << "expected original callback failure";
  } catch (const Exception& failure) {
    EXPECT_EQ(failure.code(), FOUNDRY_LOCAL_ERROR_INTERNAL);
    EXPECT_NE(std::string(failure.what()).find("local callback exploded"), std::string::npos);
  }

  EXPECT_TRUE(request.IsCompleted());
  EXPECT_EQ(session->TurnCount(), 0);
  session->SetStreamingCallback(nullptr, nullptr);
  session->ProcessRequest(request, response);
  EXPECT_EQ(session->TurnCount(), 1);
}

TEST_F(SdkSessionTest, HostedAdapterRejectsCallbackMutationsAndRetainsCapturedPreflightAfterSessionRelease) {
  auto context = ClientContext();
  auto session = api::CreateHostedSession(context, identity_);
  LocalCallback callback = [&](auto data) {
    EXPECT_THROW(session->UndoTurns(0), Exception);
    std::string ignored;
    CollectLocal(data, ignored);
    return 0;
  };
  session->SetStreamingCallback(InvokeLocal, &callback);
  auto request = CodingRequest();
  Response response;
  session->ProcessRequest(request, response);
  auto preflight = session->CreateRequestPreflight(request);
  session.reset();
  const auto budget = preflight->Execute();
  EXPECT_GT(budget.prompt_tokens, 0);
  EXPECT_EQ(budget.output_reserve_tokens, 17);
  EXPECT_THROW(preflight->Execute(), Exception);
}

TEST_F(SdkSessionTest, HostedAdapterCloseDuringCallbackIsLocalAndNeverRebindsItsChildren) {
  auto context = ClientContext();
  auto session = api::CreateHostedSession(context, identity_);
  auto request = CodingRequest();
  Response response;
  session->ProcessRequest(request, response);
  LocalCallback callback = [&](auto data) {
    std::string ignored;
    CollectLocal(data, ignored);
    context->Close();
    return 0;
  };
  session->SetStreamingCallback(InvokeLocal, &callback);
  EXPECT_ANY_THROW(session->ProcessRequest(request, response));
  EXPECT_TRUE(request.IsCompleted());
  EXPECT_EQ(session->TurnCount(), 1);
  EXPECT_THROW(session->ProcessRequest(request, response), Exception);
  EXPECT_FALSE(manager_->IsShutdownRequested());
  EXPECT_TRUE(manager_->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1")->IsLoaded());
}

TEST_F(SdkSessionTest, HostedAdapterPumpsLateInputAndKeepsCallbackAcknowledgmentsUsableAfterQueueFinish) {
  auto context = ClientContext();
  auto identity = objects_->AddSession(std::make_unique<QueueSession>());
  auto session = api::CreateHostedSession(context, identity);
  ItemQueue input;
  input.Push(std::make_unique<TextItem>("first"));
  Request request;
  request.AddBorrowedItem(&input);
  std::string streamed;
  bool extended = false;
  LocalCallback callback = [&](auto data) {
    CollectLocal(data, streamed);
    if (!extended) {
      extended = true;
      input.Push(std::make_unique<TextItem>("late"));
      input.MarkFinished();
    }

    return 0;
  };
  session->SetStreamingCallback(InvokeLocal, &callback);
  Response response;
  session->ProcessRequest(request, response);
  EXPECT_TRUE(extended);
  EXPECT_EQ(streamed, "firstlate");
  EXPECT_EQ(AssistantText(response), streamed);
}

TEST_F(SdkSessionTest, HostedAdapterInputEncodingFailureCancelsOnlyItsInvocationAndPreservesTheOriginalError) {
  auto context = ClientContext();
  auto identity = objects_->AddSession(std::make_unique<QueueSession>());
  auto session = api::CreateHostedSession(context, identity);
  ItemQueue input;
  input.Push(std::make_unique<ItemQueue>());
  Request request;
  request.AddBorrowedItem(&input);
  Response response;
  try {
    session->ProcessRequest(request, response);
    FAIL() << "nested live queues require a transfer identity";
  } catch (const Exception& failure) {
    EXPECT_EQ(failure.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE) << failure.what();
    EXPECT_NE(std::string(failure.what()).find("requires an invocation stream"), std::string::npos);
  }

  EXPECT_TRUE(request.IsCompleted());
  input.Push(std::make_unique<TextItem>("replacement"));
  input.MarkFinished();
  session->ProcessRequest(request, response);
  EXPECT_EQ(AssistantText(response), "replacement");
}

TEST_F(SdkSessionTest, HostedAdapterKeepsAdmittedStateAliveWhenTheCallbackReleasesItsSession) {
  auto context = ClientContext();
  auto session = api::CreateHostedSession(context, identity_);
  auto* admitted = session.get();
  LocalCallback callback = [&](auto data) {
    std::string ignored;
    CollectLocal(data, ignored);
    session.reset();
    return 0;
  };
  session->SetStreamingCallback(InvokeLocal, &callback);
  auto request = CodingRequest();
  Response response;
  try {
    admitted->ProcessRequest(request, response);
    FAIL() << "released session must cancel its active request";
  } catch (const Exception& failure) {
    EXPECT_EQ(failure.code(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED) << failure.what();
  }

  EXPECT_FALSE(session);
  EXPECT_TRUE(request.IsCompleted());
  EXPECT_FALSE(manager_->IsShutdownRequested());
}

TEST_F(SdkSessionTest, CapturedModelMetadataOutlivesItsHandleThroughSessionAndPreflightExecution) {
  auto* model = manager_->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1");
  auto captured = manager_->CaptureModel(*model);
  const auto identity = objects_->AddSession(captured->CreateSession());
  captured.reset();
  objects_->ReleaseSession(identity_);
  auto context = ClientContext();
  auto session = api::CreateHostedSession(context, identity);
  auto request = CodingRequest();
  Response response;
  session->ProcessRequest(request, response);
  EXPECT_EQ(AssistantText(response).size(), 17);
  auto preflight = session->CreateRequestPreflight(request);
  session.reset();
  EXPECT_THROW(model->Unload(), Exception);
  EXPECT_GT(preflight->Execute().prompt_tokens, 0);
  preflight.reset();
  EXPECT_NO_THROW(model->Unload());
}

TEST_F(SdkSessionTest, ModelDispatchPreservesExplicitLoadAndNativeInUseRejection) {
  auto* model = manager_->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1");
  const Json payload{{"model", EncodeModelSelection(CatalogType::kLocal, *model)}};
  EXPECT_EQ(Call(Operation::IsLoaded, payload).value, true);
  EXPECT_EQ(Call(Operation::Unload, payload).status, FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  auto other = payload;
  other["model"]["path"] = (root_ / "other-artifact").string();
  EXPECT_EQ(Call(Operation::IsLoaded, other).value, false);
  EXPECT_EQ(Call(Operation::Unload, other).status, FOUNDRY_LOCAL_OK);
  other["model"]["path"] = "";
  EXPECT_EQ(Call(Operation::IsLoaded, other).value, false);
  EXPECT_EQ(Call(Operation::Unload, other).status, FOUNDRY_LOCAL_OK);
  EXPECT_TRUE(model->IsLoaded());

  const auto io = Call(Operation::ModelInputOutputInfo, payload);
  ASSERT_EQ(io.status, FOUNDRY_LOCAL_OK) << io.error;
  EXPECT_NE(std::find_if(io.value.at("inputs").begin(), io.value.at("inputs").end(),
                         [](const auto& descriptor) { return descriptor.at("type") == FOUNDRY_LOCAL_ITEM_TEXT; }),
            io.value.at("inputs").end());
  const auto loaded = Call(Operation::LoadedModels, {{"catalog", "local"}});
  ASSERT_EQ(loaded.status, FOUNDRY_LOCAL_OK) << loaded.error;
  ASSERT_EQ(loaded.value.size(), 1);
  EXPECT_EQ(loaded.value.front(), payload.at("model"));

  ASSERT_EQ(Call(Operation::ReleaseSession, {{"object", EncodeObjectIdentity(identity_)}}).status, FOUNDRY_LOCAL_OK);
  ASSERT_EQ(Call(Operation::Unload, payload).status, FOUNDRY_LOCAL_OK);
  EXPECT_EQ(Call(Operation::IsLoaded, payload).value, false);
  EXPECT_EQ(Call(Operation::CreateSession, payload).status, FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  ASSERT_EQ(Call(Operation::Load, payload).status, FOUNDRY_LOCAL_OK);
  const auto created = Call(Operation::CreateSession, payload);
  ASSERT_EQ(created.status, FOUNDRY_LOCAL_OK) << created.error;
  auto context = ClientContext();
  auto session = api::CreateHostedSession(context, DecodeObjectIdentity(created.value));
  auto request = CodingRequest();
  Response response;
  session->ProcessRequest(request, response);
  EXPECT_EQ(AssistantText(response).size(), 17);
}

TEST_F(SdkSessionTest, ModelDispatchRejectsChangedExecutionMetadataAndArtifactWithoutBreakingTheConnection) {
  auto* model = manager_->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1");
  const Json payload{{"model", EncodeModelSelection(CatalogType::kLocal, *model)}};
  auto changed = payload;
  changed["model"]["info"]["task"] = "embeddings";
  EXPECT_EQ(Call(Operation::CreateSession, changed).status, FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  changed = payload;
  changed["model"]["path"] = (root_ / "other-artifact").string();
  EXPECT_EQ(Call(Operation::Load, changed).status, FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  changed["model"]["path"] = "relative-artifact";
  EXPECT_EQ(Call(Operation::Load, changed).status, FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  changed["model"]["path"] = "";
  EXPECT_EQ(Call(Operation::Load, changed).status, FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  EXPECT_EQ(Call(Operation::CreateSession, changed).status, FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  changed = payload;
  changed["model"]["catalog"] = "unknown";
  EXPECT_EQ(Call(Operation::CreateSession, changed).status, FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(Call(Operation::CreateSession, payload).status, FOUNDRY_LOCAL_OK);
}

TEST_F(SdkSessionTest, ModelDispatchReconcilesLiveRegistrationsAndNeverUnloadsAReplacementArtifact) {
  ModelInfo metadata;
  metadata.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "coding");
  metadata.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT, 1);
  metadata.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT, 1);
  Json registration{{"catalog", "local"},
                    {"path", (std::filesystem::path(FOUNDRY_LOCAL_TEST_DATA_DIR) / "tiny-paged-attention").string()},
                    {"id", "live-model:1"},
                    {"metadata", EncodeModelInfo(metadata)}};
  const auto added = Call(Operation::RegisterModel, registration);
  ASSERT_EQ(added.status, FOUNDRY_LOCAL_OK) << added.error;
  const auto selected = DecodeModelSelection(added.value);
  EXPECT_EQ(selected.info.task, "coding");
  EXPECT_EQ(*selected.info.GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT), 1);
  EXPECT_EQ(*selected.info.GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT), 1);
  const Json original{{"model", added.value}};
  ASSERT_EQ(Call(Operation::Load, original).status, FOUNDRY_LOCAL_OK);
  const Json removal{{"catalog", "local"}, {"alias_or_id", "live-model:1"}};
  EXPECT_EQ(Call(Operation::UnregisterModel, removal).status, FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  ASSERT_EQ(Call(Operation::Unload, original).status, FOUNDRY_LOCAL_OK);
  ASSERT_EQ(Call(Operation::UnregisterModel, removal).status, FOUNDRY_LOCAL_OK);
  EXPECT_EQ(Call(Operation::Load, original).status, FOUNDRY_LOCAL_ERROR_INVALID_USAGE);

  const auto replacement_path = root_ / "replacement";
  std::filesystem::copy(std::filesystem::path(FOUNDRY_LOCAL_TEST_DATA_DIR) / "tiny-paged-attention", replacement_path,
                        std::filesystem::copy_options::recursive);
  registration["path"] = replacement_path.string();
  const auto replacement = Call(Operation::RegisterModel, registration);
  ASSERT_EQ(replacement.status, FOUNDRY_LOCAL_OK) << replacement.error;
  const Json current{{"model", replacement.value}};
  EXPECT_EQ(Call(Operation::Load, original).status, FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  ASSERT_EQ(Call(Operation::Load, current).status, FOUNDRY_LOCAL_OK);
  EXPECT_EQ(Call(Operation::Unload, original).status, FOUNDRY_LOCAL_OK);
  EXPECT_EQ(Call(Operation::IsLoaded, original).value, false);
  EXPECT_EQ(Call(Operation::IsLoaded, current).value, true);
}

TEST_F(SdkSessionTest, FailedNativeSessionFactoryReleasesItsConstructionReference) {
  auto* model = manager_->GetCatalog(CatalogType::kLocal).GetModelVariant("tiny-rpc:1");
  objects_->ReleaseSession(identity_);
  auto metadata = model->Info();
  metadata.task = "unsupported-task";
  auto& native = Manager::Instance();
  auto invalid =
      Model::FromModelInfo(metadata, model->GetPath(), native.GetDownloadManager(), native.GetModelLoadManager());
  EXPECT_THROW(fl::Session::Create(invalid), Exception);
  EXPECT_NO_THROW(model->Unload());
}

}  // namespace
}  // namespace fl::hosting
