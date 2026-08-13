// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/session/oga_generator_cancellable.h"

#include "exception.h"
#include "inferencing/session/operation_context.h"
#include "inferencing/session/session.h"
#include "inferencing/session/session_runtime.h"
#include "internal_api/test_helpers.h"
#include "model.h"
#include "model_info.h"
#include "telemetry/telemetry_logger.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

/// Fake generator whose GenerateNextToken always fails. `session_terminated` models what the real
/// OgaGenerator reports after terminate_session: only when it is true may TryGenerateNextToken treat the
/// failure as an expected stop.
class ThrowingGenerator {
 public:
  void GenerateNextToken() {
    ++call_count;
    throw std::runtime_error(message);
  }

  bool IsSessionTerminated() const { return session_terminated; }

  int call_count = 0;
  bool session_terminated = false;
  std::string message = "engine failure";
};

class StopThenThrowGenerator {
 public:
  StopThenThrowGenerator(const fl::OperationContext& operation, bool report_terminated)
      : operation_(operation), report_terminated_(report_terminated) {}

  void GenerateNextToken() {
    ++call_count;
    operation_.RequestStop(fl::StopReason::kExternalCancel);
    session_terminated_ = report_terminated_;
    throw std::runtime_error(message);
  }

  bool IsSessionTerminated() const { return session_terminated_; }

  int call_count = 0;
  std::string message = "engine failure after stop raced";

 private:
  const fl::OperationContext& operation_;
  const bool report_terminated_;
  bool session_terminated_ = false;
};

class RawCallCounter {
 public:
  void SetInputs(int& /*tensors*/) { ++set_inputs_count; }
  void GenerateNextToken() { ++generate_count; }

  std::vector<int32_t> GetNextTokens() {
    ++get_tokens_count;
    return {42};
  }

  bool IsDone() {
    ++is_done_count;
    return false;
  }

  bool IsSessionTerminated() const { return false; }

  int set_inputs_count = 0;
  int generate_count = 0;
  int get_tokens_count = 0;
  int is_done_count = 0;
};

class DecodeCallCounter {
 public:
  const char* Decode(int32_t /*token*/) {
    ++decode_count;
    return "decoded";
  }

  int decode_count = 0;
};

/// Fake generator that blocks inside GenerateNextToken until it is cancelled, then fails exactly the way
/// ORT GenAI does once terminate_session has been delivered.
class BlockingGenerator : public fl::ICancellable {
 public:
  void GenerateNextToken() {
    std::unique_lock<std::mutex> lock(mutex_);

    if (!canceled_cv_.wait_for(lock, std::chrono::seconds(5), [this] { return canceled_; })) {
      throw std::logic_error("deadline watchdog did not cancel the published generator");
    }

    throw std::runtime_error("session terminated");
  }

  /// noexcept, matching ICancellable: cancellation is delivered from framework boundaries that cannot
  /// handle a failure.
  bool Cancel() noexcept override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      canceled_ = true;
      terminated_ = true;
    }

    canceled_cv_.notify_all();
    return true;
  }

  bool IsSessionTerminated() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return terminated_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable canceled_cv_;
  bool canceled_ = false;
  bool terminated_ = false;
};

/// Model-free runtime: it publishes a fake generator through ActiveGenerator and drives it exactly the way a
/// modality does, without ever touching the model lease (which is left empty).
class RawGeneratorRuntime : public fl::SessionRuntime {
 public:
  RawGeneratorRuntime(const fl::Model& catalog_model, fl::ILogger& logger, fl::ITelemetry& telemetry)
      : SessionRuntime(catalog_model, logger, telemetry, fl::ModelSessionLease{}) {}

  fl::SessionType Type() const override { return fl::SessionType::kChat; }

  /// True once TryGenerateNextToken absorbed the engine's termination error instead of letting it escape.
  bool StoppedWithoutThrowing() const { return stopped_without_throwing_; }

 private:
  void ProcessRequestImpl(const fl::OperationContext& operation, const fl::Request& /*request*/,
                          fl::Response& /*response*/) override {
    fl::ActiveGenerator active(operation, generator_);
    stopped_without_throwing_ = !fl::TryGenerateNextToken(generator_, operation);
  }

  BlockingGenerator generator_;
  bool stopped_without_throwing_ = false;
};

/// Drives one operation to completion so the raw helper can be exercised against a real stop latch.
class StoppedOperation {
 public:
  explicit StoppedOperation(fl::StopReason reason) {
    state_ = std::make_shared<fl::OperationState>(std::weak_ptr<fl::SessionRuntime>{},
                                                  std::weak_ptr<fl::RequestControl>{}, std::nullopt,
                                                  std::chrono::milliseconds(0));
    context_ = std::make_unique<fl::OperationContext>(state_);
    state_->RequestStop(reason);
  }

  const fl::OperationContext& Context() const { return *context_; }

  /// Publish `generator` long enough for the stop to be delivered to it, exactly as a modality would.
  void DeliverEngineCancel(fl::ICancellable& generator) {
    fl::ActiveGenerator active(Context(), generator);
  }

 private:
  std::shared_ptr<fl::OperationState> state_;
  std::unique_ptr<fl::OperationContext> context_;
};

/// An operation that was never stopped.
class RunningOperation {
 public:
  RunningOperation() {
    state_ = std::make_shared<fl::OperationState>(std::weak_ptr<fl::SessionRuntime>{},
                                                  std::weak_ptr<fl::RequestControl>{}, std::nullopt,
                                                  std::chrono::milliseconds(0));
    context_ = std::make_unique<fl::OperationContext>(state_);
  }

  const fl::OperationContext& Context() const { return *context_; }

 private:
  std::shared_ptr<fl::OperationState> state_;
  std::unique_ptr<fl::OperationContext> context_;
};

TEST(OgaGeneratorCancellationTest, PreStoppedOperationDoesNotEnterGeneratorAfterDeliveredCancel) {
  StoppedOperation operation(fl::StopReason::kExternalCancel);

  // Registering after the stop delivers engine cancellation through a lease and outside every lock.
  BlockingGenerator blocking;
  operation.DeliverEngineCancel(blocking);

  ThrowingGenerator generator;
  generator.session_terminated = false;

  EXPECT_TRUE(operation.Context().ShouldStop());
  EXPECT_TRUE(operation.Context().EngineCancelDelivered());
  EXPECT_FALSE(fl::TryGenerateNextToken(generator, operation.Context()));
  EXPECT_EQ(generator.call_count, 0);
}

TEST(OgaGeneratorCancellationTest, PreStoppedOperationSkipsEveryRawOgaBoundary) {
  StoppedOperation operation(fl::StopReason::kExternalCancel);
  RawCallCounter generator;
  DecodeCallCounter decoder;
  int tensors = 0;
  int token_count = 7;

  EXPECT_FALSE(fl::TrySetGeneratorInputs(generator, tensors, operation.Context()));
  EXPECT_FALSE(fl::TryGenerateNextToken(generator, operation.Context()));
  EXPECT_FALSE(fl::TryGetNextTokens(generator, operation.Context()).has_value());
  EXPECT_FALSE(fl::TryDecodeToken(decoder, int32_t{42}, operation.Context()).has_value());
  EXPECT_FALSE(fl::TryIncrementTokenCount(token_count, operation.Context()));
  EXPECT_TRUE(fl::IsGeneratorDoneCancellationSafe(generator, operation.Context()));

  EXPECT_EQ(generator.set_inputs_count, 0);
  EXPECT_EQ(generator.generate_count, 0);
  EXPECT_EQ(generator.get_tokens_count, 0);
  EXPECT_EQ(generator.is_done_count, 0);
  EXPECT_EQ(decoder.decode_count, 0);
  EXPECT_EQ(token_count, 7);
}

TEST(OgaGeneratorCancellationTest, StopRacingStartedCallSuppressesOnlyConfirmedTermination) {
  RunningOperation operation;
  StopThenThrowGenerator generator(operation.Context(), /*report_terminated=*/true);

  EXPECT_FALSE(fl::TryGenerateNextToken(generator, operation.Context()));
  EXPECT_EQ(generator.call_count, 1);
  EXPECT_TRUE(operation.Context().ShouldStop());
}

TEST(OgaGeneratorCancellationTest, UnrelatedErrorRacingStopIsRethrown) {
  RunningOperation operation;
  StopThenThrowGenerator generator(operation.Context(), /*report_terminated=*/false);

  EXPECT_THROW(static_cast<void>(fl::TryGenerateNextToken(generator, operation.Context())), std::runtime_error);
  EXPECT_EQ(generator.call_count, 1);
  EXPECT_FALSE(operation.Context().EngineCancelDelivered());
}

TEST(OgaGeneratorCancellationTest, DeadlineTerminationIsTranslatedToTimeoutBySession) {
  fl::test::FakeServiceBindings services;
  auto catalog_model =
      fl::Model::FromModelInfo(fl::ModelInfo{}, "", services.download_manager, services.model_load_manager);
  fl::TelemetryLogger telemetry{"test", fl::test::NullLog()};

  auto runtime = fl::MakeSessionRuntime<RawGeneratorRuntime>(catalog_model, services.logger, telemetry);
  fl::Session session(runtime);

  fl::Request request;
  request.SetTimeout(std::chrono::milliseconds(200));
  fl::Response response;

  try {
    session.ProcessRequest(request, response);
    FAIL() << "Expected timeout";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_TIMEOUT);
  }

  EXPECT_TRUE(runtime->StoppedWithoutThrowing());
  EXPECT_TRUE(request.canceled.load());
  EXPECT_TRUE(request.timed_out.load());
}

TEST(OgaGeneratorCancellationTest, RuntimeErrorFromRunningRequestIsRethrownUnchanged) {
  RunningOperation operation;

  ThrowingGenerator generator;
  generator.session_terminated = true;  // irrelevant: nothing stopped this operation
  generator.message = "unrelated engine failure";

  try {
    static_cast<void>(fl::TryGenerateNextToken(generator, operation.Context()));
    FAIL() << "Expected runtime_error";
  } catch (const std::runtime_error& ex) {
    EXPECT_STREQ(ex.what(), generator.message.c_str());
  }
}

TEST(OgaGeneratorCancellationTest, SealedOperationRejectsLaterStops) {
  RunningOperation operation;

  ASSERT_TRUE(operation.Context().TrySeal());
  EXPECT_TRUE(operation.Context().IsSealed());

  // Post-seal cancellation is a no-op: it must not latch, and must not be able to reach a generator.
  EXPECT_FALSE(operation.Context().RequestStop(fl::StopReason::kExternalCancel));
  EXPECT_FALSE(operation.Context().RequestStop(fl::StopReason::kTimeout));
  EXPECT_FALSE(operation.Context().ShouldStop());
  EXPECT_FALSE(operation.Context().EngineCancelDelivered());
}

TEST(OgaGeneratorCancellationTest, SealFailsOnceCancellationHasWon) {
  StoppedOperation operation(fl::StopReason::kTimeout);

  EXPECT_FALSE(operation.Context().TrySeal());
  EXPECT_FALSE(operation.Context().IsSealed());
  EXPECT_EQ(operation.Context().Reason(), fl::StopReason::kTimeout);
}

}  // namespace
