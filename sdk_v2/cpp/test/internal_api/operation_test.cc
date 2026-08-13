// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/session/operation.h"

#include "inferencing/session/operation_context.h"
#include "inferencing/session/operation_state.h"
#include "inferencing/session/session.h"
#include "inferencing/session/session_runtime.h"
#include "internal_api/test_helpers.h"
#include "items/text_item.h"
#include "model.h"
#include "model_info.h"
#include "telemetry/telemetry_logger.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {

using namespace std::chrono_literals;

constexpr auto kDiagnosticTimeout = 5s;

class CountingCancellable final : public fl::ICancellable {
 public:
  bool Cancel() noexcept override {
    cancel_count_.fetch_add(1, std::memory_order_relaxed);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancelled_ = true;
    }

    cancelled_cv_.notify_all();
    return true;
  }

  int CancelCount() const noexcept {
    return cancel_count_.load(std::memory_order_relaxed);
  }

  bool WaitForCancellation() {
    std::unique_lock<std::mutex> lock(mutex_);
    return cancelled_cv_.wait_for(lock, kDiagnosticTimeout, [this] { return cancelled_; });
  }

 private:
  std::atomic<int> cancel_count_{0};
  std::mutex mutex_;
  std::condition_variable cancelled_cv_;
  bool cancelled_ = false;
};

class OperationRuntime final : public fl::SessionRuntime {
 public:
  enum class Behavior {
    kComplete,
    kBlockUntilCancelled,
  };

  OperationRuntime(const fl::Model& catalog_model, fl::ILogger& logger, fl::ITelemetry& telemetry,
                   Behavior behavior)
      : SessionRuntime(catalog_model, logger, telemetry, fl::ModelSessionLease{}), behavior_(behavior) {}

  fl::SessionType Type() const override {
    return fl::SessionType::kChat;
  }

  int ProcessCount() const noexcept {
    return process_count_.load(std::memory_order_relaxed);
  }

  int GeneratorCancelCount() const noexcept {
    return generator_.CancelCount();
  }

  bool WaitUntilRunning() {
    std::unique_lock<std::mutex> lock(mutex_);
    return running_cv_.wait_for(lock, kDiagnosticTimeout, [this] { return running_; });
  }

 private:
  void ProcessRequestImpl(const fl::OperationContext& operation, const fl::Request& /*request*/,
                          fl::Response& response) override {
    process_count_.fetch_add(1, std::memory_order_relaxed);

    if (behavior_ == Behavior::kComplete) {
      fl::Response pending;
      pending.items.push_back(std::make_unique<fl::TextItem>("completed"));
      pending.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;

      if (operation.TrySeal()) {
        response.Swap(pending);
      }

      return;
    }

    {
      fl::ActiveGenerator active(operation, generator_);

      {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = true;
      }
      running_cv_.notify_all();

      if (!generator_.WaitForCancellation()) {
        throw std::runtime_error("operation cancellation was not delivered");
      }
    }

    fl::Response partial;
    partial.items.push_back(std::make_unique<fl::TextItem>("partial"));
    partial.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;
    response.Swap(partial);
  }

  const Behavior behavior_;
  std::atomic<int> process_count_{0};
  CountingCancellable generator_;
  std::mutex mutex_;
  std::condition_variable running_cv_;
  bool running_ = false;
};

class StopDeliveryGate {
 public:
  static void Pause(void* context) noexcept {
    static_cast<StopDeliveryGate*>(context)->Pause();
  }

  bool WaitUntilPaused() {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, kDiagnosticTimeout, [this] { return paused_; });
  }

  void Release() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }

    cv_.notify_all();
  }

 private:
  void Pause() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    paused_ = true;
    cv_.notify_all();
    cv_.wait(lock, [this] { return released_; });
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  bool paused_ = false;
  bool released_ = false;
};

struct ControlledClock {
  static std::chrono::steady_clock::time_point Now(void* context) noexcept {
    auto& clock = *static_cast<ControlledClock*>(context);
    clock.read_count.fetch_add(1, std::memory_order_relaxed);
    return clock.now;
  }

  std::chrono::steady_clock::time_point now;
  std::atomic<int> read_count{0};
};

class LinearizationClock {
 public:
  static std::chrono::steady_clock::time_point Now(void* context) noexcept {
    return static_cast<LinearizationClock*>(context)->Read();
  }

  explicit LinearizationClock(std::chrono::steady_clock::time_point now) : now_(now) {}

  bool WaitUntilReadStarted() {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, kDiagnosticTimeout, [this] { return read_started_; });
  }

  void MarkCancelStarted() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancel_started_ = true;
    }

    cv_.notify_all();
  }

  void MarkCancelCompleted() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancel_completed_ = true;
    }

    cv_.notify_all();
  }

  bool CancelCompletedDuringRead() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancel_completed_during_read_;
  }

 private:
  std::chrono::steady_clock::time_point Read() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    read_started_ = true;
    cv_.notify_all();

    cv_.wait_for(lock, kDiagnosticTimeout, [this] { return cancel_started_; });

    // Diagnostic timeout: while OperationState::mu_ is held by TrySeal, RequestStop cannot complete. If the
    // clock were sampled before that lock, the cancellation would complete here and this test would fail.
    cancel_completed_during_read_ =
        cv_.wait_for(lock, 250ms, [this] { return cancel_completed_; });
    return now_;
  }

  const std::chrono::steady_clock::time_point now_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool read_started_ = false;
  bool cancel_started_ = false;
  bool cancel_completed_ = false;
  bool cancel_completed_during_read_ = false;
};

class OperationTest : public ::testing::Test {
 protected:
  std::shared_ptr<OperationRuntime> MakeRuntime(OperationRuntime::Behavior behavior) {
    return fl::MakeSessionRuntime<OperationRuntime>(catalog_model_, services_.logger, telemetry_, behavior);
  }

  fl::test::FakeServiceBindings services_;
  fl::Model catalog_model_ =
      fl::Model::FromModelInfo(fl::ModelInfo{}, "", services_.download_manager, services_.model_load_manager);
  fl::TelemetryLogger telemetry_{"test", fl::test::NullLog()};
};

TEST_F(OperationTest, PreCancelledOperationSkipsRuntimeAndClearsReusedResponse) {
  auto runtime = MakeRuntime(OperationRuntime::Behavior::kComplete);
  fl::Session session(runtime);
  fl::Request request;
  auto operation = session.CreateOperation(request);

  fl::Response response;
  response.items.push_back(std::make_unique<fl::TextItem>("stale"));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_LENGTH;

  operation->Cancel();
  const auto outcome = operation->Process(response);

  EXPECT_EQ(outcome, fl::OperationOutcome::kCancelled);
  EXPECT_EQ(operation->Status(), fl::OperationStatus::kCancelled);
  EXPECT_EQ(runtime->ProcessCount(), 0);
  EXPECT_TRUE(response.items.empty());
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_NONE);
  EXPECT_TRUE(request.canceled.load());
  EXPECT_FALSE(request.timed_out.load());
}

TEST_F(OperationTest, ActiveCancelStopsExactGeneratorAndClearsPartialResponse) {
  auto runtime = MakeRuntime(OperationRuntime::Behavior::kBlockUntilCancelled);
  fl::Session session(runtime);
  fl::Request request;
  auto operation = session.CreateOperation(request);

  fl::Response response;
  response.items.push_back(std::make_unique<fl::TextItem>("stale"));

  fl::OperationOutcome outcome = fl::OperationOutcome::kCompleted;
  std::exception_ptr process_error;
  std::thread worker([&] {
    try {
      outcome = operation->Process(response);
    } catch (...) {
      process_error = std::current_exception();
    }
  });

  const bool running = runtime->WaitUntilRunning();
  operation->Cancel();
  worker.join();

  ASSERT_TRUE(running);
  ASSERT_FALSE(process_error);
  EXPECT_EQ(outcome, fl::OperationOutcome::kCancelled);
  EXPECT_EQ(operation->Status(), fl::OperationStatus::kCancelled);
  EXPECT_EQ(runtime->ProcessCount(), 1);
  EXPECT_EQ(runtime->GeneratorCancelCount(), 1);
  EXPECT_TRUE(response.items.empty());
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_NONE);
  EXPECT_TRUE(request.canceled.load());
  EXPECT_FALSE(request.timed_out.load());
}

TEST_F(OperationTest, CompletedOperationReplacesStaleResponseWithCommittedCandidate) {
  auto runtime = MakeRuntime(OperationRuntime::Behavior::kComplete);
  fl::Session session(runtime);
  fl::Request request;
  auto operation = session.CreateOperation(request);

  fl::Response response;
  response.items.push_back(std::make_unique<fl::TextItem>("stale"));
  response.finish_reason = FOUNDRY_LOCAL_FINISH_LENGTH;

  const auto outcome = operation->Process(response);

  ASSERT_EQ(outcome, fl::OperationOutcome::kCompleted);
  ASSERT_EQ(response.items.size(), 1u);
  ASSERT_EQ(response.items[0]->type, FOUNDRY_LOCAL_ITEM_TEXT);
  EXPECT_EQ(static_cast<const fl::TextItem&>(*response.items[0]).text, "completed");
  EXPECT_EQ(response.finish_reason, FOUNDRY_LOCAL_FINISH_STOP);
  EXPECT_EQ(operation->Status(), fl::OperationStatus::kCompleted);
  EXPECT_FALSE(request.canceled.load());
}

TEST(OperationStateTest, DeadlineAndSealUseOneFirstWinnerDecision) {
  using Clock = std::chrono::steady_clock;

  const auto deadline = Clock::time_point{} + 10s;
  ControlledClock expired_clock{.now = deadline};
  const fl::OperationState::TestHooks expired_hooks{
      .now = &ControlledClock::Now,
      .context = &expired_clock,
  };
  auto expired = std::make_shared<fl::OperationState>(
      std::weak_ptr<fl::SessionRuntime>{}, std::weak_ptr<fl::RequestControl>{}, deadline, 10s, &expired_hooks);

  EXPECT_FALSE(expired->TrySeal());
  EXPECT_EQ(expired->Reason(), fl::StopReason::kTimeout);
  EXPECT_EQ(expired->Status(), fl::OperationStatus::kTimedOut);
  EXPECT_FALSE(expired->RequestStop(fl::StopReason::kExternalCancel));
  EXPECT_EQ(expired_clock.read_count.load(std::memory_order_relaxed), 1);

  ControlledClock pre_deadline_clock{.now = deadline - 1ms};
  const fl::OperationState::TestHooks pre_deadline_hooks{
      .now = &ControlledClock::Now,
      .context = &pre_deadline_clock,
  };
  auto sealed = std::make_shared<fl::OperationState>(
      std::weak_ptr<fl::SessionRuntime>{}, std::weak_ptr<fl::RequestControl>{}, deadline, 10s,
      &pre_deadline_hooks);

  EXPECT_TRUE(sealed->TrySeal());
  EXPECT_TRUE(sealed->IsSealed());
  EXPECT_EQ(sealed->Reason(), fl::StopReason::kNone);
  EXPECT_FALSE(sealed->RequestStop(fl::StopReason::kTimeout));
  EXPECT_EQ(pre_deadline_clock.read_count.load(std::memory_order_relaxed), 1);
}

TEST(OperationStateTest, ClockSampleHoldsLifecycleMutexAgainstConcurrentStop) {
  using Clock = std::chrono::steady_clock;

  const auto deadline = Clock::time_point{} + 10s;
  LinearizationClock clock(deadline - 1ms);
  const fl::OperationState::TestHooks hooks{
      .now = &LinearizationClock::Now,
      .context = &clock,
  };
  auto state = std::make_shared<fl::OperationState>(
      std::weak_ptr<fl::SessionRuntime>{}, std::weak_ptr<fl::RequestControl>{}, deadline, 10s, &hooks);

  bool seal_won = false;
  std::thread sealer([&] {
    seal_won = state->TrySeal();
  });

  const bool read_started = clock.WaitUntilReadStarted();
  bool cancel_won = false;
  std::thread canceller([&] {
    clock.MarkCancelStarted();
    cancel_won = state->RequestStop(fl::StopReason::kExternalCancel);
    clock.MarkCancelCompleted();
  });

  sealer.join();
  canceller.join();

  ASSERT_TRUE(read_started);
  EXPECT_FALSE(clock.CancelCompletedDuringRead());
  EXPECT_TRUE(seal_won);
  EXPECT_FALSE(cancel_won);
  EXPECT_TRUE(state->IsSealed());
  EXPECT_EQ(state->Reason(), fl::StopReason::kNone);
}

TEST(OperationStateTest, RegistrationRacingStoppedSnapshotIsCancelledExactlyOnce) {
  StopDeliveryGate gate;
  const fl::OperationState::TestHooks hooks{
      .before_generator_drain = &StopDeliveryGate::Pause,
      .context = &gate,
  };
  auto state = std::make_shared<fl::OperationState>(
      std::weak_ptr<fl::SessionRuntime>{}, std::weak_ptr<fl::RequestControl>{}, std::nullopt, 0ms, &hooks);
  fl::OperationContext operation(state);

  bool stop_won = false;
  std::thread stopper([&] {
    stop_won = state->RequestStop(fl::StopReason::kExternalCancel);
  });

  const bool paused = gate.WaitUntilPaused();
  if (!paused) {
    gate.Release();
    stopper.join();
    FAIL() << "stop delivery did not reach the deterministic gate";
    return;
  }

  CountingCancellable generator;
  auto active = std::make_unique<fl::ActiveGenerator>(operation, generator);
  EXPECT_EQ(generator.CancelCount(), 1);

  gate.Release();
  stopper.join();

  EXPECT_TRUE(stop_won);
  EXPECT_EQ(generator.CancelCount(), 1);
  active.reset();
}

TEST(OperationStateTest, RepeatedStopCancelsPublishedGeneratorAtMostOnce) {
  auto state = std::make_shared<fl::OperationState>(
      std::weak_ptr<fl::SessionRuntime>{}, std::weak_ptr<fl::RequestControl>{}, std::nullopt, 0ms);
  fl::OperationContext operation(state);
  CountingCancellable generator;
  fl::ActiveGenerator active(operation, generator);

  EXPECT_TRUE(state->RequestStop(fl::StopReason::kExternalCancel));
  EXPECT_FALSE(state->RequestStop(fl::StopReason::kTimeout));
  EXPECT_EQ(generator.CancelCount(), 1);
}

}  // namespace
