// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for SessionManager: tracking, shutdown rejection, and session cache.

#include "inferencing/session/session_manager.h"
#include "inferencing/session/session_registration.h"
#include "inferencing/execution_provider.h"
#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/generative/openresponses/response_store.h"
#include "ep_detection/ep_detector.h"
#include "exception.h"
#include "logger.h"
#include "model.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "items/tool_result_item.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

using namespace fl;

namespace {

class ToggleThrowLogger final : public ILogger {
 public:
  void Log(LogLevel, std::string_view) override {
    if (throw_on_log) {
      throw std::runtime_error("injected logging failure");
    }
  }

  bool throw_on_log = false;
};

class SessionCacheCoordinator final : public IResponseCacheCoordinator {
 public:
  explicit SessionCacheCoordinator(SessionManager& manager) : manager_(manager) {}

  void Drop(const std::string& response_id) noexcept override { manager_.EvictCached(response_id); }

 private:
  SessionManager& manager_;
};

}  // namespace

// ===========================================================================
// Test fixture: loads the shared test model once per suite
// ===========================================================================

class SessionManagerTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    auto model_path = fl::test::GetTestModelPath(fl::test::kTestChatModelAlias);
    logger_ = std::make_unique<StderrLogger>();
    ep_detector_ = std::make_unique<test::CpuOnlyEpDetector>();
    load_manager_ = std::make_unique<ModelLoadManager>(*ep_detector_, *logger_);

    auto result = load_manager_->LoadModel(
        model_path.string(),
        fl::test::kTestChatModelAlias);

    ASSERT_EQ(result.status, ModelLoadManager::LoadStatus::kSuccess)
        << "Failed to load test model from: " << model_path;

    model_ = result.model;
  }

  static void TearDownTestSuite() {
    if (load_manager_) {
      load_manager_->UnloadModel(fl::test::kTestChatModelAlias);
    }

    load_manager_.reset();
    ep_detector_.reset();
    model_ = nullptr;
  }

  GenAIModelInstance& GetModel() { return *model_; }
  const Model& GetCatalogModel() { return catalog_model_; }
  ILogger& GetLogger() { return *logger_; }

  /// Create an unregistered ChatSession (for cache tests that only test cache mechanics).
  std::unique_ptr<ChatSession> MakeSession() {
    return std::make_unique<ChatSession>(GetCatalogModel(), GetModel(), GetLogger(), null_telemetry_);
  }

  /// Tracked session: a session + its registration guard.
  /// Destruction order: registration (second member) is destroyed before session (first member),
  /// which is correct — deregister before the session object is destroyed.
  struct TrackedSession {
    std::unique_ptr<ChatSession> session;
    SessionRegistration registration;
  };

  /// Create a registered ChatSession (for tracking tests).
  TrackedSession MakeTrackedSession(SessionManager& mgr) {
    auto session = MakeSession();
    SessionRegistration reg(mgr, *session);
    return {std::move(session), std::move(reg)};
  }

  static inline std::unique_ptr<StderrLogger> logger_;
  static inline std::unique_ptr<test::CpuOnlyEpDetector> ep_detector_;
  static inline std::unique_ptr<ModelLoadManager> load_manager_;
  static inline GenAIModelInstance* model_ = nullptr;
  static inline fl::test::FakeServiceBindings svc_;
  static inline Model catalog_model_ = Model::FromModelInfo(
      ModelInfo{}, "", svc_.download_manager, svc_.model_load_manager);
  TelemetryLogger null_telemetry_{"test", fl::test::NullLog()};
};

// ===========================================================================
// Tracking tests
// ===========================================================================

TEST_F(SessionManagerTest, RegisterAndDeregisterTracksCount) {
  SessionManager mgr(GetLogger());
  EXPECT_EQ(mgr.ActiveCount(), 0u);

  {
    auto tracked1 = MakeTrackedSession(mgr);
    EXPECT_EQ(mgr.ActiveCount(), 1u);

    auto tracked2 = MakeTrackedSession(mgr);
    EXPECT_EQ(mgr.ActiveCount(), 2u);
  }

  // Both destroyed — deregistered via SessionRegistration
  EXPECT_EQ(mgr.ActiveCount(), 0u);
}

TEST_F(SessionManagerTest, CancelAllRejectsNewRegistrations) {
  SessionManager mgr(GetLogger());
  mgr.CancelAll();

  auto session = MakeSession();
  EXPECT_THROW(SessionRegistration(mgr, *session), fl::Exception);
}

TEST_F(SessionManagerTest, WaitForDrainReturnsImmediatelyWhenEmpty) {
  SessionManager mgr(GetLogger());
  mgr.WaitForDrain(std::chrono::milliseconds(10));
  EXPECT_EQ(mgr.ActiveCount(), 0u);
}

// ===========================================================================
// Cache tests — CheckOut / CheckIn
// ===========================================================================

TEST_F(SessionManagerTest, CheckOutMissReturnsNullptr) {
  SessionManager mgr(GetLogger());
  auto result = mgr.CheckOut("nonexistent");
  EXPECT_EQ(result, nullptr);
}

TEST_F(SessionManagerTest, CheckInAndCheckOutRoundTrip) {
  SessionManager mgr(GetLogger());
  auto session = MakeSession();
  auto* raw = session.get();

  mgr.CheckIn("resp-1", std::move(session));
  EXPECT_EQ(mgr.CacheSize(), 1u);

  auto checked_out = mgr.CheckOut("resp-1");
  ASSERT_NE(checked_out, nullptr);
  EXPECT_EQ(checked_out.get(), raw);
  EXPECT_EQ(mgr.CacheSize(), 0u);
}

TEST_F(SessionManagerTest, CheckOutRemovesFromCache) {
  SessionManager mgr(GetLogger());
  auto session = MakeSession();
  mgr.CheckIn("resp-1", std::move(session));

  auto checked_out = mgr.CheckOut("resp-1");
  ASSERT_NE(checked_out, nullptr);

  // Second checkout for the same key is a miss
  auto second = mgr.CheckOut("resp-1");
  EXPECT_EQ(second, nullptr);
}

TEST_F(SessionManagerTest, CheckInReplacesExistingKey) {
  SessionManager mgr(GetLogger());

  auto session1 = MakeSession();
  auto session2 = MakeSession();
  auto* raw2 = session2.get();

  mgr.CheckIn("resp-1", std::move(session1));
  mgr.CheckIn("resp-1", std::move(session2));

  EXPECT_EQ(mgr.CacheSize(), 1u);

  auto checked_out = mgr.CheckOut("resp-1");
  EXPECT_EQ(checked_out.get(), raw2);
}

TEST_F(SessionManagerTest, CheckInFailurePreservesExistingSessionForTheSameKey) {
  ToggleThrowLogger manager_logger;
  SessionManager mgr(manager_logger);

  auto existing = MakeSession();
  auto* existing_raw = existing.get();
  mgr.CheckIn("resp-1", std::move(existing));

  manager_logger.throw_on_log = true;
  EXPECT_THROW(mgr.CheckIn("resp-1", MakeSession()), std::runtime_error);
  manager_logger.throw_on_log = false;

  EXPECT_EQ(mgr.CacheSize(), 1u);
  auto checked_out = mgr.CheckOut("resp-1");
  ASSERT_NE(checked_out, nullptr);
  EXPECT_EQ(checked_out.get(), existing_raw);
}

TEST_F(SessionManagerTest, EvictCachedRemovesEntry) {
  SessionManager mgr(GetLogger());
  mgr.CheckIn("resp-1", MakeSession());
  ASSERT_EQ(mgr.CacheSize(), 1u);

  EXPECT_TRUE(mgr.EvictCached("resp-1"));
  EXPECT_EQ(mgr.CacheSize(), 0u);

  // Second eviction is a miss — entry already gone.
  EXPECT_FALSE(mgr.EvictCached("resp-1"));
}

TEST_F(SessionManagerTest, EvictCachedUnknownKeyReturnsFalse) {
  SessionManager mgr(GetLogger());
  EXPECT_FALSE(mgr.EvictCached("never-cached"));
  EXPECT_EQ(mgr.CacheSize(), 0u);
}

TEST_F(SessionManagerTest, EvictCachedFreesLruSlot) {
  // Eviction must release the LRU list slot so a subsequent CheckIn doesn't trigger
  // capacity-based eviction of an unrelated entry.
  SessionManager mgr(GetLogger(), /*cache_capacity=*/2);

  mgr.CheckIn("resp-1", MakeSession());
  mgr.CheckIn("resp-2", MakeSession());
  ASSERT_EQ(mgr.CacheSize(), 2u);

  ASSERT_TRUE(mgr.EvictCached("resp-1"));
  EXPECT_EQ(mgr.CacheSize(), 1u);

  mgr.CheckIn("resp-3", MakeSession());
  EXPECT_EQ(mgr.CacheSize(), 2u);

  // resp-2 must still be present — only resp-1 was evicted.
  EXPECT_NE(mgr.CheckOut("resp-2"), nullptr);
}

TEST_F(SessionManagerTest, LruEvictionRemovesOldestEntry) {
  // Capacity 2 for easy testing
  SessionManager mgr(GetLogger(), /*cache_capacity=*/2);

  auto s1 = MakeSession();
  auto s2 = MakeSession();
  auto s3 = MakeSession();
  auto* raw2 = s2.get();
  auto* raw3 = s3.get();

  mgr.CheckIn("resp-1", std::move(s1));
  mgr.CheckIn("resp-2", std::move(s2));
  EXPECT_EQ(mgr.CacheSize(), 2u);

  // Adding a third should evict resp-1 (oldest)
  mgr.CheckIn("resp-3", std::move(s3));
  EXPECT_EQ(mgr.CacheSize(), 2u);

  // resp-1 was evicted
  EXPECT_EQ(mgr.CheckOut("resp-1"), nullptr);

  // resp-2 and resp-3 are still cached
  auto out2 = mgr.CheckOut("resp-2");
  EXPECT_EQ(out2.get(), raw2);

  auto out3 = mgr.CheckOut("resp-3");
  EXPECT_EQ(out3.get(), raw3);
}

TEST_F(SessionManagerTest, ResponseCapacityEvictionDropsMatchingSessionWhenLruOrdersDiverge) {
  SessionManager manager(GetLogger(), /*cache_capacity=*/3);
  SessionCacheCoordinator cache(manager);
  ResponseStore store(/*capacity=*/2, &cache);

  store.Store("resp-1", {{"id", "resp-1"}}, nlohmann::json::array());
  store.Store("resp-2", {{"id", "resp-2"}}, nlohmann::json::array());

  auto response_1 = MakeSession();
  auto response_2 = MakeSession();
  auto unrelated = MakeSession();
  auto* response_1_raw = response_1.get();
  auto* unrelated_raw = unrelated.get();
  manager.CheckIn("resp-1", std::move(response_1));
  manager.CheckIn("resp-2", std::move(response_2));
  manager.CheckIn("unrelated", std::move(unrelated));

  // Metadata now considers resp-1 most recent and resp-2 least recent. The session cache has an independent order:
  // unrelated is most recent and resp-1 is least recent.
  ASSERT_TRUE(store.Get("resp-1").has_value());
  store.Store("resp-3", {{"id", "resp-3"}}, nlohmann::json::array());

  EXPECT_EQ(manager.CheckOut("resp-2"), nullptr);
  auto retained_response_1 = manager.CheckOut("resp-1");
  auto retained_unrelated = manager.CheckOut("unrelated");
  ASSERT_NE(retained_response_1, nullptr);
  ASSERT_NE(retained_unrelated, nullptr);
  EXPECT_EQ(retained_response_1.get(), response_1_raw);
  EXPECT_EQ(retained_unrelated.get(), unrelated_raw);
}

TEST_F(SessionManagerTest, CacheCapacityOne) {
  SessionManager mgr(GetLogger(), /*cache_capacity=*/1);

  auto s1 = MakeSession();
  auto s2 = MakeSession();
  auto* raw2 = s2.get();

  mgr.CheckIn("resp-1", std::move(s1));
  mgr.CheckIn("resp-2", std::move(s2));

  EXPECT_EQ(mgr.CacheSize(), 1u);
  EXPECT_EQ(mgr.CheckOut("resp-1"), nullptr);

  auto out = mgr.CheckOut("resp-2");
  EXPECT_EQ(out.get(), raw2);
}

TEST_F(SessionManagerTest, CancelAllClearsCache) {
  SessionManager mgr(GetLogger());

  auto session = MakeSession();
  mgr.CheckIn("resp-1", std::move(session));
  EXPECT_EQ(mgr.CacheSize(), 1u);

  mgr.CancelAll();
  EXPECT_EQ(mgr.CacheSize(), 0u);
}

TEST_F(SessionManagerTest, DestructorClearsCache) {
  // Ensure no crash when SessionManager is destroyed with cached sessions.
  auto mgr = std::make_unique<SessionManager>(GetLogger());
  auto session = MakeSession();
  mgr->CheckIn("resp-1", std::move(session));

  mgr.reset();  // Should not crash — clears cache, waits for drain
}

TEST_F(SessionManagerTest, EvictedSessionIsDestroyed) {
  SessionManager mgr(GetLogger(), /*cache_capacity=*/1);

  auto s1 = MakeSession();
  auto s2 = MakeSession();

  mgr.CheckIn("resp-1", std::move(s1));
  EXPECT_EQ(mgr.CacheSize(), 1u);

  // Evicting s1 by inserting s2 into a capacity-1 cache
  mgr.CheckIn("resp-2", std::move(s2));

  // s1 was destroyed (evicted), only s2 remains cached
  EXPECT_EQ(mgr.CacheSize(), 1u);
}

TEST_F(SessionManagerTest, CheckedOutSessionNotAffectedByCheckIn) {
  SessionManager mgr(GetLogger(), /*cache_capacity=*/1);

  auto s1 = MakeSession();
  auto* raw1 = s1.get();
  mgr.CheckIn("resp-1", std::move(s1));

  // Check out — session is now out of cache
  auto checked_out = mgr.CheckOut("resp-1");
  EXPECT_EQ(checked_out.get(), raw1);

  // Insert new session — should not affect checked_out
  auto s2 = MakeSession();
  mgr.CheckIn("resp-2", std::move(s2));

  // Original session still valid
  EXPECT_NE(checked_out, nullptr);
  EXPECT_EQ(checked_out.get(), raw1);
}

// ===========================================================================
// Cancel propagation — model-free (own fixture-less tests so they never invoke
// SessionManagerTest::SetUpTestSuite, which loads a model).
// ===========================================================================

namespace {

/// Test-only Session that blocks inside ProcessRequestImpl until its request's cancel flag is
/// observed. Lets a unit test verify SessionManager::CancelAll() propagates cancellation to every
/// registered session without loading a model. Polls the atomic exactly like the real generation
/// loop, with a safety deadline so a broken Cancel() fails the test instead of hanging the suite.
class BlockingCancelSession : public Session {
 public:
  BlockingCancelSession(const Model& model, ILogger& logger, ITelemetry& telemetry)
      : Session(model, logger, telemetry) {}

  SessionType Type() const override { return SessionType::kChat; }

  bool InFlight() const { return in_flight_.load(std::memory_order_acquire); }

 protected:
  void ProcessRequestImpl(const Request& request, Response& /*response*/) override {
    in_flight_.store(true, std::memory_order_release);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!request.canceled.load(std::memory_order_relaxed)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return;  // safety net: a broken Cancel() must not hang the test suite
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }

 private:
  std::atomic<bool> in_flight_{false};
};

/// Spin until `pred` is true or the timeout elapses. Returns pred's final value.
template <typename Pred>
bool WaitUntil(Pred pred, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  return true;
}

}  // namespace

TEST(SessionManagerCancelTest, CancelAllCancelsInFlightRequestsOnEverySession) {
  fl::test::FakeServiceBindings svc;
  Model catalog_model = Model::FromModelInfo(ModelInfo{}, "", svc.download_manager, svc.model_load_manager);
  TelemetryLogger telemetry{"test", fl::test::NullLog()};
  SessionManager mgr(fl::test::NullLog());

  BlockingCancelSession s1(catalog_model, fl::test::NullLog(), telemetry);
  BlockingCancelSession s2(catalog_model, fl::test::NullLog(), telemetry);
  SessionRegistration r1(mgr, s1);
  SessionRegistration r2(mgr, s2);

  Request req1;
  Request req2;

  // Drive each session's blocking ProcessRequest on its own worker so both requests are in-flight
  // (registered in active_requests_) at the same time — exercising "every registered session".
  auto f1 = std::async(std::launch::async, [&] {
    Response resp;
    s1.ProcessRequest(req1, resp);
  });
  auto f2 = std::async(std::launch::async, [&] {
    Response resp;
    s2.ProcessRequest(req2, resp);
  });

  ASSERT_TRUE(WaitUntil([&] { return s1.InFlight() && s2.InFlight(); }, std::chrono::seconds(2)))
      << "worker requests never became in-flight";

  mgr.CancelAll();

  // CancelAll set each request's flag; the blocked workers observe it and return promptly.
  EXPECT_EQ(f1.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_EQ(f2.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_TRUE(req1.canceled.load(std::memory_order_relaxed));
  EXPECT_TRUE(req2.canceled.load(std::memory_order_relaxed));
}

TEST(SessionManagerCancelTest, RequestAdmittedAfterCancelIsStampedCanceled) {
  // Models the late-admission window: a session is registered (its streaming thread exists) but the
  // request hasn't reached ProcessRequest when the shutdown sweep runs. Cancel()'s per-request loop
  // can't see this request yet, so the latch must stamp it on insert — otherwise it would run a full
  // uncanceled turn and block JoinAll() until the 5s safety deadline.
  fl::test::FakeServiceBindings svc;
  Model catalog_model = Model::FromModelInfo(ModelInfo{}, "", svc.download_manager, svc.model_load_manager);
  TelemetryLogger telemetry{"test", fl::test::NullLog()};
  SessionManager mgr(fl::test::NullLog());

  BlockingCancelSession s(catalog_model, fl::test::NullLog(), telemetry);
  SessionRegistration r(mgr, s);

  // Cancel while no request is in-flight — this only latches session_canceled_; the per-request
  // cancel loop has nothing to flip.
  mgr.CancelAll();

  Request req;

  // Now drive the request. It is admitted after Cancel() ran, so ProcessRequest must stamp it on
  // insert and the blocking loop must observe cancellation at its first poll.
  auto f = std::async(std::launch::async, [&] {
    Response resp;
    s.ProcessRequest(req, resp);
  });

  EXPECT_EQ(f.wait_for(std::chrono::seconds(2)), std::future_status::ready)
      << "late-admitted request ran uncanceled — the session_canceled_ latch did not stamp it";
  EXPECT_TRUE(req.canceled.load(std::memory_order_relaxed));
}

namespace {

class SessionUsageTelemetry : public TelemetryLogger {
 public:
  SessionUsageTelemetry() : TelemetryLogger("test", fl::test::NullLog()) {}

  struct ActionCall {
    Action action;
    ActionStatus status;
    InvocationContext context;
    std::string model_id;
  };

  void RecordAction(Action action, ActionStatus status, const InvocationContext& context,
                    int64_t /*duration_ms*/, const std::string& model_id) override {
    std::lock_guard<std::mutex> lock(mutex);
    actions.push_back({action, status, context, model_id});
  }

  void RecordModelUsage(const ModelUsageInfo& usage) override {
    if (throw_on_usage) {
      throw std::runtime_error("test telemetry failure");
    }

    std::lock_guard<std::mutex> lock(mutex);
    usages.push_back(usage);
  }

  bool throw_on_usage = false;
  std::mutex mutex;
  std::vector<ActionCall> actions;
  std::vector<ModelUsageInfo> usages;
};

class UsageTestSession : public Session {
 public:
  UsageTestSession(const Model& model, ITelemetry& telemetry, bool concurrent = false)
      : Session(model, fl::test::NullLog(), telemetry, concurrent) {}

  SessionType Type() const override { return SessionType::kChat; }
  std::function<void(const Request&, Response&)> process;
  int additional_usage_calls = 0;
  bool throw_on_additional_usage = false;
  std::string execution_provider;

 protected:
  void ProcessRequestImpl(const Request& request, Response& response) override {
    response.usage = {.prompt_tokens = 7, .completion_tokens = 4, .total_tokens = 11, .reasoning_tokens = 1};
    response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;
    if (process) {
      process(request, response);
    }
  }

  std::string ExecutionProvider() const override { return execution_provider; }

  void RecordAdditionalModelUsage(const Response&, const ModelUsageInfo&) override {
    if (throw_on_additional_usage) {
      throw std::runtime_error("test modality telemetry failure");
    }

    ++additional_usage_calls;
  }
};

class SessionTelemetryTest : public ::testing::Test {
 protected:
  static ModelInfo MakeModelInfo() {
    ModelInfo info;
    info.model_id = "usage-model";
    info.name = "usage-model";
    info.execution_provider = "CPUExecutionProvider";
    return info;
  }

  fl::test::FakeServiceBindings svc;
  Model model = Model::FromModelInfo(MakeModelInfo(), "", svc.download_manager, svc.model_load_manager);
  SessionUsageTelemetry telemetry;
};

}  // namespace

TEST_F(SessionTelemetryTest, DirectCallsHaveDistinctContextsAndCurrentTurnMetrics) {
  UsageTestSession session(model, telemetry);
  Request request;
  Response first;
  Response second;
  session.ProcessRequest(request, first);
  session.ProcessRequest(request, second);

  ASSERT_EQ(telemetry.actions.size(), 2u);
  ASSERT_EQ(telemetry.usages.size(), 2u);
  EXPECT_NE(telemetry.actions[0].context.correlation_id, telemetry.actions[1].context.correlation_id);
  for (size_t i = 0; i < 2; ++i) {
    const auto& action = telemetry.actions[i];
    const auto& usage = telemetry.usages[i];
    EXPECT_EQ(action.action, Action::kSessionProcessRequest);
    EXPECT_EQ(action.status, ActionStatus::kSuccess);
    EXPECT_EQ(action.model_id, "usage-model");
    EXPECT_EQ(usage.model_id, "usage-model");
    EXPECT_EQ(usage.execution_provider, "CPUExecutionProvider");
    EXPECT_FALSE(action.context.indirect);
    EXPECT_FALSE(usage.indirect);
    EXPECT_FALSE(usage.stream);
    EXPECT_EQ(action.context.correlation_id.size(), 36u);
    EXPECT_EQ(usage.correlation_id, action.context.correlation_id);
    EXPECT_EQ(usage.user_agent, action.context.user_agent);
    EXPECT_EQ(usage.total_tokens, 11);
    EXPECT_EQ(usage.input_token_count, 7);
    EXPECT_EQ(usage.num_messages, 0u);
    EXPECT_EQ(usage.time_to_first_token_ms, -1);
    EXPECT_EQ(usage.memory_used_mb, -1);
    EXPECT_GE(usage.total_time_ms, 0);
  }
}

TEST_F(SessionTelemetryTest, IndirectContextIsConsumedOnceAndReplacedOnReuse) {
  UsageTestSession session(model, telemetry);
  Request request;
  const InvocationContext route{"test-client", "route-one", false};
  session.SetInvocationContext(route.AsIndirect());
  Response first;
  session.ProcessRequest(request, first);
  Response second;
  session.ProcessRequest(request, second);
  session.SetInvocationContext(InvocationContext{"other-client", "route-three", true});
  Response third;
  session.ProcessRequest(request, third);

  ASSERT_EQ(telemetry.usages.size(), 3u);
  EXPECT_EQ(telemetry.usages[0].correlation_id, "route-one");
  EXPECT_EQ(telemetry.usages[0].user_agent, "test-client");
  EXPECT_TRUE(telemetry.usages[0].indirect);
  EXPECT_FALSE(telemetry.usages[1].indirect);
  EXPECT_NE(telemetry.usages[1].correlation_id, "route-one");
  EXPECT_EQ(telemetry.usages[2].correlation_id, "route-three");
  EXPECT_EQ(telemetry.usages[2].user_agent, "other-client");
  EXPECT_TRUE(telemetry.usages[2].indirect);
}

TEST_F(SessionTelemetryTest, MovedSessionRetainsPendingContextAndCancellationLatch) {
  UsageTestSession original(model, telemetry);
  original.SetInvocationContext(InvocationContext{"client", "moved-context", true});
  original.Cancel();
  UsageTestSession session(std::move(original));
  Request request;
  Response response;
  session.ProcessRequest(request, response);

  EXPECT_TRUE(request.canceled.load());
  ASSERT_EQ(telemetry.actions.size(), 1u);
  EXPECT_EQ(telemetry.actions[0].context.correlation_id, "moved-context");
  EXPECT_EQ(telemetry.actions[0].status, ActionStatus::kCanceled);
}

TEST_F(SessionTelemetryTest, TypedAndJsonMessagesAreCountedWithoutCountingTransportItems) {
  UsageTestSession session(model, telemetry);
  session.execution_provider = "CUDAExecutionProvider";
  session.SetStreamingCallback([](flStreamingCallbackData, void*) { return 0; });
  Request request;
  request.items.push_back(nullptr);
  request.AddOwnedItem(std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_USER, "hello"));
  request.AddOwnedItem(std::make_unique<ToolResultItem>("call", "result"));
  request.AddOwnedItem(std::make_unique<TextItem>("plain text"));
  request.AddOwnedItem(std::make_unique<TextItem>(
      R"({"messages":[{"role":"user","content":"one"},{"role":"assistant","content":"two"}]})",
      FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
  Response response;
  session.ProcessRequest(request, response);

  ASSERT_EQ(telemetry.usages.size(), 1u);
  EXPECT_EQ(telemetry.usages[0].num_messages, 4u);
  EXPECT_EQ(telemetry.usages[0].execution_provider, "CUDAExecutionProvider");
  EXPECT_TRUE(telemetry.usages[0].stream);
}

TEST_F(SessionTelemetryTest, UsageSinkFailureDoesNotChangeResponseOrSuppressAdditionalUsage) {
  UsageTestSession session(model, telemetry);
  telemetry.throw_on_usage = true;
  Request request;
  Response response;
  EXPECT_NO_THROW(session.ProcessRequest(request, response));
  EXPECT_EQ(response.usage.total_tokens, 11);
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
  EXPECT_EQ(session.additional_usage_calls, 1);
  ASSERT_EQ(telemetry.actions.size(), 1u);
  EXPECT_EQ(telemetry.actions[0].status, ActionStatus::kSuccess);
}

TEST_F(SessionTelemetryTest, AdditionalUsageFailureDoesNotChangeResponse) {
  UsageTestSession session(model, telemetry);
  session.throw_on_additional_usage = true;
  Request request;
  Response response;
  EXPECT_NO_THROW(session.ProcessRequest(request, response));
  EXPECT_EQ(response.usage.total_tokens, 11);
  ASSERT_EQ(telemetry.usages.size(), 1u);
  ASSERT_EQ(telemetry.actions.size(), 1u);
  EXPECT_EQ(telemetry.actions[0].status, ActionStatus::kSuccess);
}

TEST_F(SessionTelemetryTest, FailedInferenceKeepsOriginalExceptionAndDoesNotEmitUsage) {
  UsageTestSession session(model, telemetry);
  session.SetInvocationContext(InvocationContext{"client", "failed-context", true});
  session.process = [](const Request&, Response&) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid test request");
  };
  Request request;
  Response response;
  try {
    session.ProcessRequest(request, response);
    FAIL() << "Expected original inference exception";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    EXPECT_NE(std::string(ex.what()).find("invalid test request"), std::string::npos);
  }

  ASSERT_EQ(telemetry.actions.size(), 1u);
  EXPECT_EQ(telemetry.actions[0].status, ActionStatus::kClientError);
  EXPECT_EQ(telemetry.actions[0].context.correlation_id, "failed-context");
  EXPECT_TRUE(telemetry.usages.empty());
  session.process = nullptr;
  session.ProcessRequest(request, response);
  ASSERT_EQ(telemetry.actions.size(), 2u);
  EXPECT_FALSE(telemetry.actions[1].context.indirect);
}

TEST_F(SessionTelemetryTest, TelemetryTokenNarrowingDoesNotWrapOrChangeResponseAccounting) {
  UsageTestSession session(model, telemetry);
  session.process = [](const Request&, Response& response) {
    response.usage.total_tokens = std::numeric_limits<int64_t>::max();
    response.usage.prompt_tokens = -1;
  };
  Request request;
  Response response;
  session.ProcessRequest(request, response);

  EXPECT_EQ(response.usage.total_tokens, std::numeric_limits<int64_t>::max());
  EXPECT_EQ(response.usage.prompt_tokens, -1);
  ASSERT_EQ(telemetry.usages.size(), 1u);
  EXPECT_EQ(telemetry.usages[0].total_tokens, std::numeric_limits<int32_t>::max());
  EXPECT_EQ(telemetry.usages[0].input_token_count, 0);
}

TEST(SessionTelemetryProviderTest, ImplicitCpuHasConcreteTelemetryNameWithoutChangingRuntimeSentinels) {
  EXPECT_EQ(EPUtils::EPtoTelemetryName(ExecutionProvider::kDefault, ""), "CPUExecutionProvider");
  EXPECT_EQ(EPUtils::EPtoGenAI(ExecutionProvider::kDefault), "");
  EXPECT_EQ(EPUtils::EPtoRegistrationName(ExecutionProvider::kDefault), "");
  EXPECT_EQ(EPUtils::StringtoEP(""), ExecutionProvider::kUnknown);
}

TEST(SessionTelemetryProviderTest, DefaultSelectionUsesConfiguredProviderInsteadOfAssumingCpu) {
  EXPECT_EQ(EPUtils::EPtoTelemetryName(ExecutionProvider::kDefault, "cpu"), "CPUExecutionProvider");
  EXPECT_EQ(EPUtils::EPtoTelemetryName(ExecutionProvider::kDefault, "CPUExecutionProvider"), "CPUExecutionProvider");
  EXPECT_EQ(EPUtils::EPtoTelemetryName(ExecutionProvider::kDefault, "cuda"), "CUDAExecutionProvider");
  EXPECT_EQ(EPUtils::EPtoTelemetryName(ExecutionProvider::kDefault, "CUDAExecutionProvider"), "CUDAExecutionProvider");
  EXPECT_EQ(EPUtils::EPtoTelemetryName(ExecutionProvider::kDefault, "WebGPU"), "WebGpuExecutionProvider");
  EXPECT_EQ(EPUtils::EPtoTelemetryName(ExecutionProvider::kDefault, "OpenVINO"), "OpenVINOExecutionProvider");
}

TEST(SessionTelemetryProviderTest, ExplicitProviderOverridesConfigWithoutChangingGenAiNames) {
  EXPECT_EQ(EPUtils::EPtoTelemetryName(ExecutionProvider::kCPU, "cuda"), "CPUExecutionProvider");
  EXPECT_EQ(EPUtils::EPtoTelemetryName(ExecutionProvider::kCUDA, "OpenVINO"), "CUDAExecutionProvider");
  EXPECT_EQ(EPUtils::EPtoGenAI(ExecutionProvider::kCPU), "");
  EXPECT_EQ(EPUtils::EPtoGenAI(ExecutionProvider::kCUDA), "cuda");
}

TEST(SessionTelemetryProviderTest, UnknownProvidersAreNotReportedAsImplicitCpu) {
  EXPECT_EQ(EPUtils::EPtoTelemetryName(ExecutionProvider::kUnknown, ""), "");
  EXPECT_EQ(EPUtils::EPtoTelemetryName(ExecutionProvider::kDefault, "unknown-provider"), "");
}
