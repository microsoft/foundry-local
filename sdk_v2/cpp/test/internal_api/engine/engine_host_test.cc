// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/engine/engine_host.h"
#include "exception.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fl {
namespace {

struct FakeRequest final : EngineBackend::Request {
  explicit FakeRequest(std::vector<int32_t> initial_tokens) : submitted_tokens(std::move(initial_tokens)) {}

  std::vector<int32_t> submitted_tokens;
  std::vector<int32_t> continuation_tokens;
  std::vector<int32_t> next_output;
  bool next_turn_complete{false};
  bool removed{false};
};

struct FakeBackendTrace {
  std::vector<std::string> calls;
  std::mutex calls_mutex;
  std::atomic<int> active_calls{0};
  std::atomic<int> max_active_calls{0};
};

class FakeEngineBackend final : public EngineBackend {
 public:
  explicit FakeEngineBackend(std::shared_ptr<FakeBackendTrace> trace) : trace_(std::move(trace)) {}

  std::unique_ptr<Request> CreateRequest(OgaGeneratorParams&, std::span<const int32_t>) override {
    throw std::logic_error("Fake requests are prepared without model-bound generator params");
  }

  void Add(Request& request) override {
    Record("add");
    if (fail_add) {
      throw std::runtime_error("add failed");
    }

    requests.push_back(&AsFake(request));
  }

  std::unique_ptr<ReadyRequest> Step() override {
    Record("step");
    if (ready.empty()) {
      return nullptr;
    }

    auto* request = ready.front();
    ready.erase(ready.begin());
    auto tokens = std::move(request->next_output);
    request->next_output.clear();
    return std::make_unique<ReadyRequest>(
        ReadyRequest{.request = request, .tokens = std::move(tokens), .is_turn_complete = request->next_turn_complete});
  }

  void Continue(Request& request, std::span<const int32_t> tokens) override {
    Record("continue");
    auto& fake_request = AsFake(request);
    fake_request.continuation_tokens.assign(tokens.begin(), tokens.end());
  }

  void Remove(Request& request) override {
    Record("remove");
    if (fail_remove) {
      throw std::runtime_error("remove failed");
    }

    AsFake(request).removed = true;
  }

  void MakeReady(FakeRequest& request, std::vector<int32_t> output, bool turn_complete) {
    request.next_output = std::move(output);
    request.next_turn_complete = turn_complete;
    ready.push_back(&request);
  }

  std::vector<FakeRequest*> requests;
  std::vector<FakeRequest*> ready;
  bool fail_add{false};
  bool fail_remove{false};

 private:
  class CallScope {
   public:
    explicit CallScope(FakeEngineBackend& backend) : backend_(backend) {
      const auto active = ++backend_.trace_->active_calls;
      auto maximum = backend_.trace_->max_active_calls.load();
      while (active > maximum && !backend_.trace_->max_active_calls.compare_exchange_weak(maximum, active)) {
      }
      std::this_thread::yield();
    }

    ~CallScope() {
      --backend_.trace_->active_calls;
    }

   private:
    FakeEngineBackend& backend_;
  };

  void Record(std::string call) {
    CallScope scope(*this);
    std::lock_guard<std::mutex> lock(trace_->calls_mutex);
    trace_->calls.push_back(std::move(call));
  }

  static FakeRequest& AsFake(Request& request) {
    return static_cast<FakeRequest&>(request);
  }

  std::shared_ptr<FakeBackendTrace> trace_;
};

struct HostFixture {
  HostFixture() {
    trace = std::make_shared<FakeBackendTrace>();
    auto owned_backend = std::make_unique<FakeEngineBackend>(trace);
    backend = owned_backend.get();
    host = std::make_unique<EngineHost>(std::move(owned_backend));
  }

  std::shared_ptr<EngineRequest> Submit(std::vector<int32_t> tokens,
                                        EngineRequestMode mode = EngineRequestMode::kStateless) {
    auto request = std::make_unique<FakeRequest>(std::move(tokens));
    last_request = request.get();
    return host->SubmitPrepared(std::move(request), mode);
  }

  FakeEngineBackend* backend;
  FakeRequest* last_request{nullptr};
  std::shared_ptr<FakeBackendTrace> trace;
  std::unique_ptr<EngineHost> host;
};

TEST(EngineHostTest, SubmitAddsPreparedRequestAndStepDemultiplexesOpaqueRequest) {
  HostFixture fixture;
  auto first = fixture.Submit({1, 2});
  auto* first_backend = fixture.last_request;
  auto second = fixture.Submit({3, 4});
  auto* second_backend = fixture.last_request;
  fixture.backend->MakeReady(*second_backend, {40, 41}, false);
  fixture.backend->MakeReady(*first_backend, {20}, true);

  EXPECT_EQ(fixture.host->Step(), second);
  EXPECT_EQ(fixture.host->Step(), first);
  EXPECT_EQ(first_backend->submitted_tokens, (std::vector<int32_t>{1, 2}));
  EXPECT_EQ(second_backend->submitted_tokens, (std::vector<int32_t>{3, 4}));
  EXPECT_EQ(std::count(fixture.trace->calls.begin(), fixture.trace->calls.end(), "add"), 2);
  EXPECT_EQ(second->DrainGeneratedTokens(), (std::vector<int32_t>{40, 41}));
  EXPECT_EQ(first->DrainGeneratedTokens(), (std::vector<int32_t>{20}));
  EXPECT_FALSE(second->IsTurnComplete());
  EXPECT_TRUE(first->IsTurnComplete());
  EXPECT_FALSE(second->IsClosed());
  EXPECT_TRUE(first->IsClosed());
  EXPECT_TRUE(first_backend->removed);
}

TEST(EngineHostTest, StatelessCompletedRequestReleasesKvAndPreservesTokensUntilDrained) {
  HostFixture fixture;
  auto request = fixture.Submit({1});
  auto* backend_request = fixture.last_request;
  fixture.backend->MakeReady(*backend_request, {10, 11}, false);
  EXPECT_EQ(fixture.host->Step(), request);
  fixture.backend->MakeReady(*backend_request, {12}, true);
  EXPECT_EQ(fixture.host->Step(), request);

  EXPECT_EQ(request->DrainGeneratedTokens(), (std::vector<int32_t>{10, 11, 12}));
  EXPECT_TRUE(request->DrainGeneratedTokens().empty());
  EXPECT_TRUE(request->IsClosed());
  EXPECT_TRUE(backend_request->removed);
}

TEST(EngineHostTest, PopGeneratedTokenPreservesOrderAndReportsAvailability) {
  HostFixture fixture;
  auto request = fixture.Submit({1});
  auto* backend_request = fixture.last_request;
  fixture.backend->MakeReady(*backend_request, {10, 11}, true);
  fixture.host->Step();

  EXPECT_TRUE(request->HasGeneratedTokens());
  EXPECT_EQ(request->PopGeneratedToken(), 10);
  EXPECT_TRUE(request->HasGeneratedTokens());
  EXPECT_EQ(request->PopGeneratedToken(), 11);
  EXPECT_FALSE(request->HasGeneratedTokens());
  EXPECT_EQ(request->PopGeneratedToken(), std::nullopt);
}

TEST(EngineHostTest, ContinueRequiresCompletedTurnAndPreservesUnreadOutput) {
  HostFixture fixture;
  auto request = fixture.Submit({1}, EngineRequestMode::kResident);
  auto* backend_request = fixture.last_request;
  EXPECT_THROW(request->Continue(std::vector<int32_t>{2}), Exception);
  fixture.backend->MakeReady(*backend_request, {10}, true);
  fixture.host->Step();

  request->Continue(std::vector<int32_t>{2, 3});

  EXPECT_EQ(backend_request->continuation_tokens, (std::vector<int32_t>{2, 3}));
  EXPECT_FALSE(request->IsTurnComplete());
  EXPECT_FALSE(request->IsClosed());
  EXPECT_FALSE(backend_request->removed);
  EXPECT_EQ(request->DrainGeneratedTokens(), (std::vector<int32_t>{10}));
}

TEST(EngineHostTest, EmptyContinuationIsRejectedWithoutCallingBackend) {
  HostFixture fixture;
  auto request = fixture.Submit({1}, EngineRequestMode::kResident);
  auto* backend_request = fixture.last_request;
  fixture.backend->MakeReady(*backend_request, {}, true);
  fixture.host->Step();
  const auto calls_before = fixture.trace->calls;

  EXPECT_THROW(request->Continue(std::span<const int32_t>{}), Exception);
  EXPECT_EQ(fixture.trace->calls, calls_before);
}

TEST(EngineHostTest, CloseRemovesExactlyOnceAndKeepsBufferedOutputDrainable) {
  HostFixture fixture;
  auto request = fixture.Submit({1}, EngineRequestMode::kResident);
  auto* backend_request = fixture.last_request;
  fixture.backend->MakeReady(*backend_request, {9}, true);
  fixture.host->Step();

  request->Close();
  request->Close();

  EXPECT_TRUE(request->IsClosed());
  EXPECT_TRUE(backend_request->removed);
  EXPECT_EQ(std::count(fixture.trace->calls.begin(), fixture.trace->calls.end(), "remove"), 1);
  EXPECT_EQ(request->DrainGeneratedTokens(), (std::vector<int32_t>{9}));
  EXPECT_THROW(request->Continue(std::vector<int32_t>{2}), Exception);
}

TEST(EngineHostTest, ShutdownRemovesAllRequestsAndRejectsFurtherWork) {
  HostFixture fixture;
  auto first = fixture.Submit({1});
  auto* first_backend = fixture.last_request;
  auto second = fixture.Submit({2});
  auto* second_backend = fixture.last_request;

  fixture.host->Shutdown();
  fixture.host->Shutdown();

  EXPECT_TRUE(first->IsClosed());
  EXPECT_TRUE(second->IsClosed());
  EXPECT_TRUE(first_backend->removed);
  EXPECT_TRUE(second_backend->removed);
  EXPECT_EQ(std::count(fixture.trace->calls.begin(), fixture.trace->calls.end(), "remove"), 2);
  EXPECT_THROW(fixture.host->Step(), Exception);
}

TEST(EngineHostTest, ShutdownSurfacesRemovalFailureAndCanBeRetried) {
  HostFixture fixture;
  auto request = fixture.Submit({1});
  auto* backend_request = fixture.last_request;
  fixture.backend->fail_remove = true;

  EXPECT_THROW(fixture.host->Shutdown(), std::runtime_error);
  EXPECT_FALSE(request->IsClosed());
  EXPECT_FALSE(backend_request->removed);

  fixture.backend->fail_remove = false;
  fixture.host->Shutdown();
  EXPECT_TRUE(request->IsClosed());
  EXPECT_TRUE(backend_request->removed);
}

TEST(EngineHostTest, CloseSurfacesBackendErrorAndRemainsRetryable) {
  HostFixture fixture;
  auto request = fixture.Submit({1});
  auto* backend_request = fixture.last_request;
  fixture.backend->fail_remove = true;

  EXPECT_THROW(request->Close(), std::runtime_error);
  EXPECT_FALSE(request->IsClosed());
  EXPECT_FALSE(backend_request->removed);

  fixture.backend->fail_remove = false;
  request->Close();
  EXPECT_TRUE(request->IsClosed());
  EXPECT_TRUE(backend_request->removed);
}

TEST(EngineHostTest, SubmitRollsBackOwnershipWhenBackendAddFails) {
  HostFixture fixture;
  fixture.backend->fail_add = true;

  EXPECT_THROW(fixture.Submit({1}), std::runtime_error);

  fixture.backend->fail_add = false;
  auto request = fixture.Submit({2});
  EXPECT_FALSE(request->IsClosed());
}

TEST(EngineHostTest, UnknownReadyRequestIsReportedAsError) {
  HostFixture fixture;
  auto request = fixture.Submit({1});
  static_cast<void>(request);
  FakeRequest unknown({2});
  fixture.backend->MakeReady(unknown, {3}, true);

  EXPECT_THROW(fixture.host->Step(), Exception);
}

TEST(EngineHostTest, BackendLifecycleCallsAreSerializedAcrossThreads) {
  HostFixture fixture;
  fixture.Submit({1});

  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&] {
      fixture.host->Step();
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(fixture.trace->max_active_calls.load(), 1);
}

}  // namespace
}  // namespace fl
