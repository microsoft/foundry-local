// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "api/hosted_backend.h"
#include "catalog.h"
#include "configuration.h"
#include "exception.h"
#include "hosting/host_server.h"
#include "hosting/sdk_values.h"
#include "inferencing/session/request.h"
#include "inferencing/session/response.h"
#include "items/message_item.h"
#include "model_info.h"
#include "manager.h"
#include "util/scope_guard.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <thread>
#include <vector>

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
#include <httplib.h>
#endif

namespace fl::hosting {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;
using State = HostLifetime::State;

class HostServerTest : public testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<uint64_t> sequence{0};
    const auto suffix = std::to_string(CurrentProcessIdentity().pid) + "-" + std::to_string(sequence.fetch_add(1));
    const auto scratch = std::filesystem::path(FOUNDRY_LOCAL_TEST_SCRATCH_DIR);
    ASSERT_TRUE(scratch.is_absolute());
    std::filesystem::create_directories(scratch);
    ASSERT_FALSE(std::filesystem::is_symlink(scratch));
    root_ = scratch / ("host-server-" + suffix);
    ASSERT_TRUE(std::filesystem::create_directory(root_));
    owns_root_ = true;
#ifdef _WIN32
    endpoint_ = LR"(\\.\pipe\foundry-native-host-test-)" + std::filesystem::path(suffix).native();
#else
    std::filesystem::permissions(root_, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    socket_directory_ = test::MakePrivateIpcDirectory();
    endpoint_ = socket_directory_ / "s";
#endif
    now_.store(std::chrono::steady_clock::now().time_since_epoch().count());
    Configuration config;
    config.app_name = "host-server-test";
    config.app_data_dir = (root_ / "data").string();
    config.model_cache_dir = (root_ / "models").string();
    config.logs_dir = (root_ / "logs").string();
    config.disable_nonessential_telemetry = true;
    manager_ = api::CreateEmbeddedBackend(config);
    ModelInfo info;
    info.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "coding");
    model_ = manager_->GetCatalog(CatalogType::kLocal)
                 .RegisterModel((std::filesystem::path(FOUNDRY_LOCAL_TEST_DATA_DIR) / "tiny-paged-attention").string(),
                                "host-model:1", info);
    lifetime_ = std::make_shared<HostLifetime>(Now() + 30s);
    Manager::Instance().SetSharedHostLifetime(lifetime_);
    server_ = std::make_unique<HostServer>(manager_, endpoint_, "test-profile", "native-test-incarnation",
                                           CurrentProcessIdentity(), lifetime_, [this] { return Now(); });
  }

  void TearDown() override {
    server_.reset();
    manager_.reset();
    if (!socket_directory_.empty()) {
      std::filesystem::remove_all(socket_directory_);
    }
    if (owns_root_) {
      std::filesystem::remove_all(root_);
    }
  }

  HostLifetime::TimePoint Now() const {
    return HostLifetime::TimePoint{std::chrono::steady_clock::duration{now_.load()}};
  }

  void Advance(std::chrono::steady_clock::duration duration) { now_.fetch_add(duration.count()); }

  bool WaitFor(State state, size_t owners = 0) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
      const auto snapshot = server_->Lifetime().Poll(Now());
      if (snapshot.state == state && snapshot.owner_count == owners) {
        return true;
      }

      std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  std::shared_ptr<SdkConnection> NewConnection() const {
    return std::make_shared<SdkConnection>(Connect(endpoint_), SdkConnection::Side::Client);
  }

  Json HandshakePayload(bool owner) const {
    const auto process = CurrentProcessIdentity();
    return {{"protocol", SdkConnection::kProtocolVersion},
            {"profile", "test-profile"},
            {"owner", owner},
            {"process", {{"pid", process.pid}, {"generation", process.generation}}}};
  }

  std::shared_ptr<api::HostedContext> Attach(bool owner = true) const {
    auto connection = NewConnection();
    const auto result = connection->Call(Operation::Handshake, HandshakePayload(owner));
    if (result.status != 0) {
      throw std::runtime_error(result.error);
    }

    return std::make_shared<api::HostedContext>(connection, result.value.at("incarnation").get<std::string>(),
                                                result.value.at("connection").get<uint64_t>());
  }

  static Request CodingRequest() {
    Request request;
    request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "Remember the word sapphire."));
    request.options.Add("max_output_tokens", "17");
    request.options.Add("temperature", "0");
    return request;
  }

  void ExpectStopped() {
    EXPECT_TRUE(server_->WaitUntilStopped(std::chrono::steady_clock::now() + 5s));
    EXPECT_NO_THROW(server_->RethrowFailure());
    EXPECT_TRUE(manager_->IsShutdownRequested());
  }

  std::atomic<std::chrono::steady_clock::duration::rep> now_{0};
  std::filesystem::path root_;
  std::filesystem::path endpoint_;
  std::filesystem::path socket_directory_;
  bool owns_root_ = false;
  std::shared_ptr<api::ManagerBackend> manager_;
  std::shared_ptr<HostLifetime> lifetime_;
  api::ModelBackend* model_ = nullptr;
  std::unique_ptr<HostServer> server_;
};

TEST_F(HostServerTest, ObservationHasNoOwnershipAndCannotLoadOrExtendUnusedStartup) {
  auto observer = Attach(false);
  const auto status = observer->Call(Operation::Observe, {});
  EXPECT_EQ(status.at("owners"), 0);
  EXPECT_EQ(status.at("state"), static_cast<int>(State::kAwaitingFirstOwner));
  const Json selection{{"model", EncodeModelSelection(CatalogType::kLocal, *model_)}};
  EXPECT_EQ(observer->Call(Operation::IsLoaded, selection), false);
  EXPECT_THROW(observer->Call(Operation::Load, selection), Exception);
  EXPECT_FALSE(model_->IsLoaded());
  Advance(30s);
  ExpectStopped();
  EXPECT_THROW(observer->Call(Operation::Observe, {}), std::exception);
}

TEST_F(HostServerTest, InvalidProfileProtocolAndPeerIdentityNeverAcquireAnOwner) {
  auto wrong_profile = NewConnection();
  auto payload = HandshakePayload(true);
  payload["profile"] = "another-profile";
  const auto incompatible = wrong_profile->Call(Operation::Handshake, payload);
  EXPECT_EQ(incompatible.status, FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  EXPECT_NE(incompatible.error.find("incompatible"), std::string::npos);
  auto wrong_protocol = NewConnection();
  payload = HandshakePayload(true);
  payload["protocol"] = SdkConnection::kProtocolVersion + 1;
  EXPECT_EQ(wrong_protocol->Call(Operation::Handshake, payload).status, FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  auto wrong_process = NewConnection();
  payload = HandshakePayload(true);
  payload["process"]["pid"] = CurrentProcessIdentity().pid + 1;
  EXPECT_EQ(wrong_process->Call(Operation::Handshake, payload).status, FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(server_->Lifetime().Poll(Now()).owner_count, 0u);
  auto valid = Attach();
  EXPECT_EQ(valid->Call(Operation::Observe, {}).at("owners"), 1);
}

TEST_F(HostServerTest, NormalHostedApisShareWeightsButKeepSessionsAndDisposalIndependent) {
  auto first = Attach();
  auto second = Attach();
  auto first_manager = api::CreateHostedBackend(manager_, [first](bool) { return first; });
  auto second_manager = api::CreateHostedBackend(manager_, [second](bool) { return second; });
  auto* first_model = first_manager->GetCatalog(CatalogType::kLocal).GetModelVariant("host-model:1");
  auto* second_model = second_manager->GetCatalog(CatalogType::kLocal).GetModelVariant("host-model:1");
  first_model->Load();
  EXPECT_TRUE(second_model->IsLoaded());
  auto first_session = first_model->CreateSession();
  auto second_session = second_model->CreateSession();
  auto request = CodingRequest();
  Response first_response;
  first_session->ProcessRequest(request, first_response);
  EXPECT_EQ(first_response.usage.completion_tokens, 17);
  EXPECT_EQ(first_session->TurnCount(), 1u);
  EXPECT_EQ(second_session->TurnCount(), 0u);
  EXPECT_THROW(second_model->Unload(), Exception);
  first_manager->Shutdown();
  ASSERT_TRUE(WaitFor(State::kAccepting, 1));
  EXPECT_FALSE(manager_->IsShutdownRequested());
  EXPECT_THROW(first_session->UndoTurns(1), Exception);
  first_session.reset();
  Response second_response;
  second_session->ProcessRequest(request, second_response);
  EXPECT_EQ(second_response.usage.completion_tokens, 17);
  EXPECT_EQ(second_session->TurnCount(), 1u);
  second_session.reset();
  second_manager->Shutdown();
  ASSERT_TRUE(WaitFor(State::kGrace));
  Advance(5s);
  ExpectStopped();
}

TEST_F(HostServerTest, ReconnectCancelsGraceButOldConnectionObjectIdsNeverRebind) {
  auto first = Attach();
  model_->Load();
  const Json selection{{"model", EncodeModelSelection(CatalogType::kLocal, *model_)}};
  const auto previous = first->Call(Operation::CreateSession, selection);
  first->Close();
  ASSERT_TRUE(WaitFor(State::kGrace));
  Advance(4s);
  auto successor = Attach();
  Advance(1h);
  ASSERT_TRUE(WaitFor(State::kAccepting, 1));
  EXPECT_THROW(successor->Call(Operation::UndoTurns, {{"object", previous}, {"count", 1}}), Exception);
  const auto current = successor->Call(Operation::CreateSession, selection);
  EXPECT_NE(current.at("connection"), previous.at("connection"));
  successor->Call(Operation::ReleaseSession, {{"object", current}});
  successor->Close();
  ASSERT_TRUE(WaitFor(State::kGrace));
  Advance(5s);
  ExpectStopped();
}

TEST_F(HostServerTest, LastOwnerLossCancelsAnUnacknowledgedNativeStreamBeforeShutdown) {
  auto connection = NewConnection();
  ASSERT_EQ(connection->Call(Operation::Handshake, HandshakePayload(true)).status, 0);
  model_->Load();
  const auto session =
      connection->Call(Operation::CreateSession, {{"model", EncodeModelSelection(CatalogType::kLocal, *model_)}});
  ASSERT_EQ(session.status, 0) << session.error;
  auto request = CodingRequest();
  const Json payload{{"object", session.value}, {"request", EncodeRequest(request, {})}, {"streaming", true}};
  std::promise<void> streamed;
  auto arrived = streamed.get_future();
  std::atomic<bool> signalled{false};
  auto running = std::async(std::launch::async, [&] {
    return connection->Call(Operation::ProcessRequest, payload, [&](auto, const Json&) {
      if (!signalled.exchange(true)) {
        streamed.set_value();
      }
    });
  });
  ScopeGuard close([&]() noexcept { connection->Close(); });
  ASSERT_EQ(arrived.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(server_->Lifetime().Poll(Now()).ipc_work_count, 1u);
  connection->Close();
  ASSERT_EQ(running.wait_for(5s), std::future_status::ready);
  EXPECT_THROW(running.get(), std::exception);
  ASSERT_TRUE(WaitFor(State::kGrace));
  EXPECT_EQ(server_->Lifetime().Poll(Now()).ipc_work_count, 0u);
  Advance(5s);
  ExpectStopped();
}

TEST_F(HostServerTest, ConcurrentCallLimitRejectsAcrossClientsAndCancellationRestoresCapacity) {
  auto connection = NewConnection();
  ASSERT_EQ(connection->Call(Operation::Handshake, HandshakePayload(true)).status, 0);
  auto other = Attach();
  model_->Load();
  const auto session =
      connection->Call(Operation::CreateSession, {{"model", EncodeModelSelection(CatalogType::kLocal, *model_)}});
  ASSERT_EQ(session.status, 0) << session.error;
  auto request = CodingRequest();
  const Json payload{{"object", session.value}, {"request", EncodeRequest(request, {})}, {"streaming", true}};
  auto other_manager = api::CreateHostedBackend(manager_, [other](bool) { return other; });
  auto* other_model = other_manager->GetCatalog(CatalogType::kLocal).GetModelVariant("host-model:1");
  auto disposable_session = other_model->CreateSession();
  auto disposable_preflight = disposable_session->CreateRequestPreflight(request);
  std::promise<void> streamed;
  auto arrived = streamed.get_future();
  std::vector<SdkConnection::InvocationId> ids;
  std::vector<std::future<SdkResult>> running;
  ScopeGuard close([&]() noexcept { connection->Close(); });
  for (std::size_t i = 0; i < HostServer::kMaxConcurrentCalls; ++i) {
    const auto id = connection->AllocateInvocation();
    ids.push_back(id);
    // No callback acknowledgment: the first request waits in streaming, the rest wait for the same session.
    running.push_back(std::async(std::launch::async, [&, id, i] {
      return connection->Call(id, Operation::ProcessRequest, payload, [&](auto, const Json&) {
        if (i == 0) {
          streamed.set_value();
        }
      });
    }));
    if (i == 0) {
      ASSERT_EQ(arrived.wait_for(5s), std::future_status::ready);
    }
  }

  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (lifetime_->Poll(Now()).ipc_work_count < HostServer::kMaxConcurrentCalls &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
  }

  ASSERT_EQ(lifetime_->Poll(Now()).ipc_work_count, HostServer::kMaxConcurrentCalls);
  try {
    other->Call(Operation::Observe, {});
    FAIL() << "Another client bypassed the host-wide worker limit";
  } catch (const Exception& error) {
    EXPECT_EQ(error.code(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
    EXPECT_NE(std::string(error.what()).find("busy"), std::string::npos);
  }

  disposable_session.reset();
  disposable_preflight.reset();
  EXPECT_EQ(lifetime_->Poll(Now()).ipc_work_count, HostServer::kMaxConcurrentCalls);
  connection->Cancel(ids.front());
  ASSERT_EQ(running.front().wait_for(5s), std::future_status::ready);
  EXPECT_EQ(running.front().get().status, FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  bool recovered = false;
  const auto recovery_deadline = std::chrono::steady_clock::now() + 5s;
  do {
    try {
      EXPECT_EQ(other->Call(Operation::Observe, {}).at("owners"), 2);
      recovered = true;
    } catch (const Exception&) {
      std::this_thread::sleep_for(5ms);
    }
  } while (!recovered && std::chrono::steady_clock::now() < recovery_deadline);
  EXPECT_TRUE(recovered);
  for (std::size_t i = 1; i < ids.size(); ++i) {
    connection->Cancel(ids[i]);
  }

  for (std::size_t i = 1; i < running.size(); ++i) {
    ASSERT_EQ(running[i].wait_for(5s), std::future_status::ready);
    EXPECT_EQ(running[i].get().status, FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  }

  EXPECT_EQ(connection->Call(Operation::ReleaseSession, {{"object", session.value}}).status, 0);
  EXPECT_NO_THROW(other_model->Unload());
  EXPECT_FALSE(other_model->IsLoaded());
  EXPECT_EQ(other->Call(Operation::Observe, {}).at("owners"), 2);
}

TEST_F(HostServerTest, ConnectionLimitIncludesUnauthenticatedPeersAndDisconnectRestoresCapacity) {
  std::vector<Channel> peers;
  for (std::size_t i = 0; i < HostServer::kMaxConnections; ++i) {
    peers.push_back(Connect(endpoint_));
  }

  EXPECT_THROW(Attach(), std::exception);
  peers.front().Close();
  std::shared_ptr<api::HostedContext> owner;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  do {
    try {
      owner = Attach();
    } catch (const std::exception&) {
      std::this_thread::sleep_for(5ms);
    }
  } while (!owner && std::chrono::steady_clock::now() < deadline);
  ASSERT_NE(owner, nullptr);
  EXPECT_EQ(owner->Call(Operation::Observe, {}).at("owners"), 1);
}

TEST_F(HostServerTest, IndependentAdmittedHttpWorkDrainsBeforeGraceAndNormalNativeShutdown) {
  auto owner = Attach();
  auto http = server_->Lifetime().TryAdmitHttpWork(Now());
  ASSERT_TRUE(http);
  owner->Close();
  ASSERT_TRUE(WaitFor(State::kDraining));
  Advance(1h);
  EXPECT_FALSE(server_->WaitUntilStopped(std::chrono::steady_clock::now() + 30ms));
  EXPECT_FALSE(manager_->IsShutdownRequested());
  http.Complete(Now());
  ASSERT_TRUE(WaitFor(State::kGrace));
  Advance(5s);
  ExpectStopped();
}

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
TEST_F(HostServerTest, NativeHttpUsesTheOwnerControllerRejectsGlobalShutdownAndKeepsStopScoped) {
  auto first = Attach();
  auto second = Attach();
  const auto urls = first->Call(Operation::WebServiceStart, {});
  ASSERT_EQ(urls.size(), 1u);
  EXPECT_EQ(second->Call(Operation::WebServiceStart, {}), urls);
  EXPECT_EQ(first->Call(Operation::WebServiceStart, {}), urls);
  httplib::Client http(urls.at(0).get<std::string>());
  http.set_keep_alive(true);
  http.set_read_timeout(5, 0);
  const auto status = http.Get("/status");
  ASSERT_TRUE(status);
  EXPECT_EQ(status->status, 200);
  const auto shutdown = http.Post("/shutdown", "", "application/json");
  ASSERT_TRUE(shutdown);
  EXPECT_EQ(shutdown->status, 403);
  EXPECT_FALSE(manager_->IsShutdownRequested());
  const auto inventory = http.Get("/catalogs/local/models/loaded?details=true");
  ASSERT_TRUE(inventory);
  EXPECT_EQ(inventory->status, 200);
  EXPECT_TRUE(Json::parse(inventory->body).is_array());
  EXPECT_THROW(first->Call(Operation::WebServiceStop, {}), Exception);
  second->Close();
  ASSERT_TRUE(WaitFor(State::kAccepting, 1));
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (lifetime_->Poll(Now()).http_work_count != 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
  }

  ASSERT_EQ(lifetime_->Poll(Now()).http_work_count, 0u);
  {
    auto active_http = lifetime_->TryAdmitHttpWork(Now());
    ASSERT_TRUE(active_http);
    EXPECT_THROW(first->Call(Operation::WebServiceStop, {}), Exception);
  }

  first->Call(Operation::WebServiceStop, {});
  EXPECT_TRUE(first->Call(Operation::WebServiceUrls, {}).empty());
  EXPECT_EQ(first->Call(Operation::Observe, {}).at("owners"), 1);
  first->Call(Operation::WebServiceStart, {});
  const auto restarted = first->Call(Operation::WebServiceUrls, {});
  httplib::Client unowned_http(restarted.at(0).get<std::string>());
  unowned_http.set_read_timeout(5, 0);
  first->Close();
  ASSERT_TRUE(WaitFor(State::kGrace));
  const auto unowned = unowned_http.Get("/status");
  ASSERT_TRUE(unowned);
  EXPECT_EQ(unowned->status, 503);
  Advance(6s);
  ExpectStopped();
}
#endif

}  // namespace
}  // namespace fl::hosting
