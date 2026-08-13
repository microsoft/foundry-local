// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for SessionManager: tracking, shutdown rejection, and session cache.

#include "inferencing/session/session_manager.h"
#include "inferencing/session/session_registration.h"
#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/model_load_manager.h"
#include "ep_detection/ep_detector.h"
#include "exception.h"
#include "logger.h"
#include "model.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace fl;

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
