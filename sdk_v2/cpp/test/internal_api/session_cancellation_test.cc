// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "c_api_types.h"
#include "exception.h"
#include "inferencing/session/session.h"
#include "internal_api/c_api_test_helpers.h"
#include "internal_api/test_helpers.h"
#include "model.h"
#include "model_info.h"
#include "telemetry/telemetry_logger.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace {
using namespace std::chrono_literals;
constexpr auto kWait = 5s;
class Gate {
 public:
  void Open() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      open_ = true;
    }

    cv_.notify_all();
  }

  bool Wait(std::chrono::milliseconds timeout = kWait) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return open_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool open_ = false;
};
template <typename Predicate>
bool WaitUntil(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + kWait;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }

    std::this_thread::yield();
  }

  return true;
}

template <typename Callable>
flErrorCode CodeOf(Callable&& callable) {
  try {
    std::forward<Callable>(callable)();
    return FOUNDRY_LOCAL_OK;
  } catch (const fl::Exception& ex) {
    return ex.code();
  }
}

flErrorCode TakeCode(const flApi& api, flStatus* status) {
  if (!status) {
    return FOUNDRY_LOCAL_OK;
  }

  const auto code = api.Status_GetErrorCode(status);
  api.Status_Release(status);
  return code;
}

class GateGenerator : public fl::ICancellable {
 public:
  void Cancel() override {
    cancel_count_.fetch_add(1, std::memory_order_relaxed);
    released.Open();
  }

  int CancelCount() const { return cancel_count_.load(std::memory_order_relaxed); }

  Gate released;

 private:
  std::atomic<int> cancel_count_{0};
};

class BlockingCancelGenerator : public fl::ICancellable {
 public:
  void Cancel() override {
    started.Open();
    static_cast<void>(allow_return.Wait());
    returned.Open();
  }

  Gate started;
  Gate allow_return;
  Gate returned;
};

class ScriptedSession : public fl::Session {
 public:
  using Body = std::function<void(const fl::Request&, fl::Response&)>;

  ScriptedSession(const fl::Model& model, fl::ILogger& logger, fl::ITelemetry& telemetry, Body body,
                  bool concurrent = false)
      : Session(model, logger, telemetry, concurrent),
        body_(std::move(body)),
        type_(concurrent ? fl::SessionType::kEmbeddings : fl::SessionType::kChat) {}

  fl::SessionType Type() const override { return type_; }
  int Entries() const { return entries_.load(std::memory_order_relaxed); }
  class PublishedGenerator {
   public:
    PublishedGenerator(const fl::Request& request, fl::ICancellable& generator) : guard_(request, generator) {}

   private:
    ActiveGenerator guard_;
  };

 private:
  void ProcessRequestImpl(const fl::Request& request, fl::Response& response) override {
    entries_.fetch_add(1, std::memory_order_relaxed);
    body_(request, response);
  }

  Body body_;
  fl::SessionType type_;
  std::atomic<int> entries_{0};
};

struct Harness {
  fl::test::FakeServiceBindings services;
  fl::Model model =
      fl::Model::FromModelInfo(fl::ModelInfo{}, "", services.download_manager, services.model_load_manager);
  fl::TelemetryLogger telemetry{"test", fl::test::NullLog()};
};

TEST(SessionCancellationTest, QueuedRequestTimeoutIncludesAdmissionWait) {
  Harness harness;
  Gate entered;
  Gate release;
  ScriptedSession session(harness.model, harness.services.logger, harness.telemetry,
                          [&](const fl::Request&, fl::Response&) {
                            entered.Open();
                            static_cast<void>(release.Wait());
                          });

  fl::Request first_request;
  fl::Response first_response;
  auto first = std::async(std::launch::async, [&] { session.ProcessRequest(first_request, first_response); });
  ASSERT_TRUE(entered.Wait());

  fl::Request queued_request;
  queued_request.SetTimeout(100ms);
  fl::Response queued_response;
  auto queued = std::async(std::launch::async, [&] {
    return CodeOf([&] { session.ProcessRequest(queued_request, queued_response); });
  });

  ASSERT_EQ(queued.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(queued.get(), FOUNDRY_LOCAL_ERROR_TIMEOUT);
  EXPECT_EQ(session.Entries(), 1);
  EXPECT_TRUE(queued_request.timed_out.load(std::memory_order_relaxed));

  release.Open();
  first.get();
}

TEST(SessionCancellationTest, QueuedRequestCancelReturnsBeforeInference) {
  Harness harness;
  Gate entered;
  Gate release;
  ScriptedSession session(harness.model, harness.services.logger, harness.telemetry,
                          [&](const fl::Request&, fl::Response&) {
                            entered.Open();
                            static_cast<void>(release.Wait());
                          });

  fl::Request first_request;
  fl::Response first_response;
  auto first = std::async(std::launch::async, [&] { session.ProcessRequest(first_request, first_response); });
  ASSERT_TRUE(entered.Wait());

  fl::Request queued_request;
  fl::Response queued_response;
  auto queued = std::async(std::launch::async, [&] {
    return CodeOf([&] { session.ProcessRequest(queued_request, queued_response); });
  });

  ASSERT_TRUE(WaitUntil([&] { return queued_request.ActiveCancellationState() != nullptr; }));
  queued_request.Cancel();

  ASSERT_EQ(queued.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(queued.get(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  EXPECT_EQ(session.Entries(), 1);

  release.Open();
  first.get();
}

TEST(SessionCancellationTest, RequestCancelInterruptsActiveGeneratorWithoutPublishingResponse) {
  Harness harness;
  Gate entered;
  GateGenerator generator;
  std::atomic<bool> committed{false};
  ScriptedSession session(harness.model, harness.services.logger, harness.telemetry,
                          [&](const fl::Request& request, fl::Response&) {
                            ScriptedSession::PublishedGenerator published(request, generator);
                            entered.Open();
                            static_cast<void>(generator.released.Wait());
                            committed.store(request.TryBeginCompletion(), std::memory_order_relaxed);
                          });

  const auto* api = fl::test::GetApi();
  const auto* inference = api->GetInferenceApi();
  fl::Request request;
  flResponse* response = nullptr;
  auto run = std::async(std::launch::async, [&] {
    return TakeCode(*api, inference->Session_ProcessRequest(AsHandle<flSession>(&session),
                                                           AsHandle<flRequest>(&request), &response));
  });

  ASSERT_TRUE(entered.Wait());
  ASSERT_EQ(inference->Request_Cancel(AsHandle<flRequest>(&request)), nullptr);
  ASSERT_EQ(run.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(run.get(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  EXPECT_EQ(generator.CancelCount(), 1);
  EXPECT_FALSE(committed.load(std::memory_order_relaxed));
  EXPECT_EQ(response, nullptr);
}

TEST(SessionCancellationTest, DistinctEmbeddingsRequestsCancelIndependently) {
  Harness harness;
  Gate both_entered;
  Gate release_survivor;
  std::atomic<int> entered{0};
  const fl::Request* canceled_request = nullptr;
  ScriptedSession session(
      harness.model, harness.services.logger, harness.telemetry,
      [&](const fl::Request& request, fl::Response&) {
        if (entered.fetch_add(1, std::memory_order_relaxed) == 1) {
          both_entered.Open();
        }

        if (&request == canceled_request) {
          static_cast<void>(WaitUntil([&] { return request.ShouldStop(); }));
        } else {
          static_cast<void>(release_survivor.Wait());
        }
      },
      true);

  fl::Request first_request;
  fl::Request second_request;
  canceled_request = &first_request;
  fl::Response first_response;
  fl::Response second_response;
  auto first = std::async(std::launch::async, [&] {
    return CodeOf([&] { session.ProcessRequest(first_request, first_response); });
  });
  auto second = std::async(std::launch::async, [&] {
    return CodeOf([&] { session.ProcessRequest(second_request, second_response); });
  });

  ASSERT_TRUE(both_entered.Wait());
  first_request.Cancel();
  ASSERT_EQ(first.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(first.get(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  EXPECT_EQ(second.wait_for(100ms), std::future_status::timeout);

  release_survivor.Open();
  EXPECT_EQ(second.get(), FOUNDRY_LOCAL_OK);
}

TEST(SessionCancellationTest, SessionCancelStopsAllInvocationsAndRejectsFutureCalls) {
  Harness harness;
  Gate both_entered;
  std::atomic<int> entered{0};
  ScriptedSession session(
      harness.model, harness.services.logger, harness.telemetry,
      [&](const fl::Request& request, fl::Response&) {
        if (entered.fetch_add(1, std::memory_order_relaxed) == 1) {
          both_entered.Open();
        }

        static_cast<void>(WaitUntil([&] { return request.ShouldStop(); }));
      },
      true);

  fl::Request first_request;
  fl::Request second_request;
  fl::Response first_response;
  fl::Response second_response;
  auto first = std::async(std::launch::async, [&] {
    return CodeOf([&] { session.ProcessRequest(first_request, first_response); });
  });
  auto second = std::async(std::launch::async, [&] {
    return CodeOf([&] { session.ProcessRequest(second_request, second_response); });
  });

  ASSERT_TRUE(both_entered.Wait());
  session.Cancel();
  EXPECT_NO_THROW(session.Cancel());
  EXPECT_EQ(first.get(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  EXPECT_EQ(second.get(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);

  fl::Request later_request;
  fl::Response later_response;
  EXPECT_EQ(CodeOf([&] { session.ProcessRequest(later_request, later_response); }),
            FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
}

TEST(SessionCancellationTest, GeneratorLifetimeExtendsUntilCancelReturns) {
  Harness harness;
  Gate entered;
  Gate unregistered;
  BlockingCancelGenerator generator;
  ScriptedSession session(harness.model, harness.services.logger, harness.telemetry,
                          [&](const fl::Request& request, fl::Response&) {
                            {
                              ScriptedSession::PublishedGenerator published(request, generator);
                              entered.Open();
                              static_cast<void>(generator.started.Wait());
                            }

                            unregistered.Open();
                          });

  fl::Request request;
  fl::Response response;
  auto run = std::async(std::launch::async, [&] {
    return CodeOf([&] { session.ProcessRequest(request, response); });
  });

  ASSERT_TRUE(entered.Wait());
  auto cancel = std::async(std::launch::async, [&] { request.Cancel(); });
  ASSERT_TRUE(generator.started.Wait());
  EXPECT_FALSE(unregistered.Wait(100ms));

  generator.allow_return.Open();
  cancel.get();
  EXPECT_EQ(run.get(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  EXPECT_TRUE(generator.returned.Wait());
  EXPECT_TRUE(unregistered.Wait());
}

TEST(SessionCancellationTest, CanceledRequestCanBeReusedAndIdleCancelIsNoOp) {
  Harness harness;
  Gate first_entered;
  std::atomic<int> invocation{0};
  ScriptedSession session(harness.model, harness.services.logger, harness.telemetry,
                          [&](const fl::Request& request, fl::Response&) {
                            if (invocation.fetch_add(1, std::memory_order_relaxed) == 0) {
                              first_entered.Open();
                              static_cast<void>(WaitUntil([&] { return request.ShouldStop(); }));
                            }
                          });

  fl::Request request;
  fl::Response response;
  auto first = std::async(std::launch::async, [&] {
    return CodeOf([&] { session.ProcessRequest(request, response); });
  });

  ASSERT_TRUE(first_entered.Wait());
  request.Cancel();
  EXPECT_EQ(first.get(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  EXPECT_TRUE(request.canceled.load(std::memory_order_relaxed));

  request.Cancel();
  EXPECT_EQ(CodeOf([&] { session.ProcessRequest(request, response); }), FOUNDRY_LOCAL_OK);
  EXPECT_FALSE(request.canceled.load(std::memory_order_relaxed));
  EXPECT_EQ(session.Entries(), 2);
}

TEST(SessionCancellationTest, TimeoutOutsideChronoRangeIsRejected) {
  const auto* api = fl::test::GetApi();
  const auto* inference = api->GetInferenceApi();
  flRequest* request = nullptr;
  ASSERT_EQ(inference->Request_Create(&request), nullptr);

  fl::test::StatusGuard status{
      inference->Request_SetTimeoutMs(request, (std::numeric_limits<uint64_t>::max)()), api};
  ASSERT_NE(status.s, nullptr);
  EXPECT_EQ(api->Status_GetErrorCode(status.s), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);

  inference->Request_Release(request);
}

}  // namespace
