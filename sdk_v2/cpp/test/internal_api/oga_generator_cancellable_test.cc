// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/session/oga_generator_cancellable.h"

#include "exception.h"
#include "inferencing/session/session.h"
#include "internal_api/test_helpers.h"
#include "model.h"
#include "model_info.h"
#include "telemetry/telemetry_logger.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {

class ThrowingGenerator {
 public:
  void GenerateNextToken() {
    ++call_count;
    throw std::runtime_error(message);
  }

  int call_count = 0;
  std::string message = "engine failure";
};

class BlockingGenerator : public fl::ICancellable {
 public:
  void GenerateNextToken() {
    std::unique_lock<std::mutex> lock(mutex_);

    if (!canceled_cv_.wait_for(lock, std::chrono::seconds(5), [this] { return canceled_; })) {
      throw std::logic_error("deadline watchdog did not cancel the published generator");
    }

    throw std::runtime_error("session terminated");
  }

  void Cancel() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      canceled_ = true;
    }

    canceled_cv_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable canceled_cv_;
  bool canceled_ = false;
};

class RawGeneratorSession : public fl::Session {
 public:
  RawGeneratorSession(const fl::Model& catalog_model, fl::ILogger& logger, fl::ITelemetry& telemetry)
      : Session(catalog_model, logger, telemetry) {}

  fl::SessionType Type() const override { return fl::SessionType::kChat; }

  /// True once TryGenerateNextToken absorbed the engine's termination error instead of letting it escape.
  bool StoppedWithoutThrowing() const { return stopped_without_throwing_; }

 private:
  void ProcessRequestImpl(const fl::Request& request, fl::Response& /*response*/) override {
    ActiveGenerator active(*this, generator_);
    stopped_without_throwing_ = !fl::TryGenerateNextToken(generator_, request);
  }

  BlockingGenerator generator_;
  bool stopped_without_throwing_ = false;
};

TEST(OgaGeneratorCancellationTest, RuntimeErrorAfterRequestCancellationStopsGeneration) {
  fl::Request request;
  request.canceled.store(true);
  ThrowingGenerator generator;

  EXPECT_FALSE(fl::TryGenerateNextToken(generator, request));
  EXPECT_EQ(generator.call_count, 1);
}

TEST(OgaGeneratorCancellationTest, DeadlineTerminationIsTranslatedToTimeoutBySession) {
  fl::test::FakeServiceBindings services;
  auto catalog_model =
      fl::Model::FromModelInfo(fl::ModelInfo{}, "", services.download_manager, services.model_load_manager);
  fl::TelemetryLogger telemetry{"test", fl::test::NullLog()};
  RawGeneratorSession session(catalog_model, services.logger, telemetry);

  fl::Request request;
  request.SetTimeout(std::chrono::milliseconds(200));
  fl::Response response;

  try {
    session.ProcessRequest(request, response);
    FAIL() << "Expected timeout";
  } catch (const fl::Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_TIMEOUT);
  }

  EXPECT_TRUE(session.StoppedWithoutThrowing());
  EXPECT_TRUE(request.canceled.load());
  EXPECT_TRUE(request.timed_out.load());
}

TEST(OgaGeneratorCancellationTest, RuntimeErrorFromRunningRequestIsRethrownUnchanged) {
  fl::Request request;
  ThrowingGenerator generator;
  generator.message = "unrelated engine failure";

  try {
    static_cast<void>(fl::TryGenerateNextToken(generator, request));
    FAIL() << "Expected runtime_error";
  } catch (const std::runtime_error& ex) {
    EXPECT_STREQ(ex.what(), generator.message.c_str());
  }
}

}  // namespace
