// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/engine_chat_generator.h"

#include "inferencing/generative/engine/engine_host.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fl {
namespace {

std::vector<int32_t> CopyTokens(std::span<const int32_t> tokens) {
  return std::vector<int32_t>(tokens.begin(), tokens.end());
}

struct AdapterRequest final : EngineBackend::Request {
  std::vector<int32_t> continuation_tokens;
  bool removed{false};
};

class AdapterBackend final : public EngineBackend {
 public:
  struct Ready {
    AdapterRequest* request;
    std::vector<int32_t> tokens;
    bool complete;
  };

  std::unique_ptr<Request> CreateRequest(OgaGeneratorParams&, std::span<const int32_t>) override {
    throw std::logic_error("model-free tests submit prepared requests");
  }

  void Add(Request&) override {}

  std::unique_ptr<ReadyRequest> Step() override {
    if (fail_step) {
      throw std::runtime_error("step failed");
    }

    if (ready.empty()) {
      return nullptr;
    }

    auto next = std::move(ready.front());
    ready.erase(ready.begin());
    return std::make_unique<ReadyRequest>(
        ReadyRequest{.request = next.request, .tokens = std::move(next.tokens), .is_turn_complete = next.complete});
  }

  void Continue(Request& request, std::span<const int32_t> tokens) override {
    auto& adapter_request = static_cast<AdapterRequest&>(request);
    adapter_request.continuation_tokens.assign(tokens.begin(), tokens.end());
  }

  void Remove(Request& request) override {
    static_cast<AdapterRequest&>(request).removed = true;
    ++remove_count;
  }

  std::vector<Ready> ready;
  bool fail_step{false};
  int remove_count{0};
};

struct AdapterFixture {
  AdapterFixture() {
    auto owned_backend = std::make_unique<AdapterBackend>();
    backend = owned_backend.get();
    host = std::make_shared<EngineHost>(std::move(owned_backend));
  }

  std::pair<std::shared_ptr<EngineRequest>, AdapterRequest*> Submit(
      EngineRequestMode mode = EngineRequestMode::kStateless) {
    auto prepared = std::make_unique<AdapterRequest>();
    auto* raw = prepared.get();
    return {host->SubmitPrepared(std::move(prepared), mode), raw};
  }

  std::unique_ptr<EngineChatGenerator> MakeGenerator(
      std::shared_ptr<EngineRequest> request,
      std::vector<int32_t> prompt_tokens,
      EngineChatGeneratorMode mode = EngineChatGeneratorMode::kStateless) {
    return std::make_unique<EngineChatGenerator>(
        host, std::move(request), [](int32_t token) { return std::to_string(token) + ","; },
        std::move(prompt_tokens), mode);
  }

  AdapterBackend* backend;
  std::shared_ptr<EngineHost> host;
};

TEST(EngineChatGeneratorRoutingTest, RequiresDynamicBatchingTextModelAndTextRequest) {
  GenAIConfig config;
  EXPECT_FALSE(ShouldUseEngineChatGenerator(config, false, false));

  config.engine = GenAIConfig::Engine{};
  config.engine->dynamic_batching = GenAIConfig::Engine::DynamicBatching{};

  EXPECT_TRUE(ShouldUseEngineChatGenerator(config, false, false));
  EXPECT_FALSE(ShouldUseEngineChatGenerator(config, true, false));
  EXPECT_FALSE(ShouldUseEngineChatGenerator(config, false, true));
}

TEST(ResidentTokenReconciliationTest, ExactMatchReturnsEmptySuffix) {
  const std::vector<int32_t> resident{1, 2, 3};
  const std::vector<int32_t> complete_prompt{1, 2, 3};

  const auto suffix = ReconcileResidentTokenSuffix(resident, complete_prompt);

  ASSERT_TRUE(suffix.has_value());
  EXPECT_TRUE(suffix->empty());
}

TEST(ResidentTokenReconciliationTest, ExactPrefixReturnsCompleteSuffix) {
  const std::vector<int32_t> resident{1, 2, 3};
  const std::vector<int32_t> complete_prompt{1, 2, 3, 7, 8};

  const auto suffix = ReconcileResidentTokenSuffix(resident, complete_prompt);

  ASSERT_TRUE(suffix.has_value());
  EXPECT_EQ(std::vector<int32_t>(suffix->begin(), suffix->end()),
            (std::vector<int32_t>{7, 8}));
}

TEST(ResidentTokenReconciliationTest, MismatchIsRejected) {
  const std::vector<int32_t> resident{1, 2, 4};
  const std::vector<int32_t> complete_prompt{1, 2, 3, 4};

  EXPECT_FALSE(ReconcileResidentTokenSuffix(resident, complete_prompt).has_value());
}

TEST(ResidentTokenReconciliationTest, ShorterCompletePromptIsRejected) {
  const std::vector<int32_t> resident{1, 2, 3};
  const std::vector<int32_t> complete_prompt{1, 2};

  EXPECT_FALSE(ReconcileResidentTokenSuffix(resident, complete_prompt).has_value());
}

TEST(ResidentTokenReconciliationTest, EmptyResidentReturnsEntirePrompt) {
  const std::vector<int32_t> resident;
  const std::vector<int32_t> complete_prompt{5, 6};

  const auto suffix = ReconcileResidentTokenSuffix(resident, complete_prompt);

  ASSERT_TRUE(suffix.has_value());
  EXPECT_EQ(std::vector<int32_t>(suffix->begin(), suffix->end()), complete_prompt);
}

TEST(EngineChatGeneratorTest, SharedHostDemultiplexesBufferedTokensWithoutLoss) {
  AdapterFixture fixture;
  auto [first_request, first_backend] = fixture.Submit();
  auto [second_request, second_backend] = fixture.Submit();
  fixture.backend->ready = {
      {.request = second_backend, .tokens = {20}, .complete = false},
      {.request = first_backend, .tokens = {10, 11}, .complete = true},
      {.request = second_backend, .tokens = {21}, .complete = true},
  };

  auto first = fixture.MakeGenerator(std::move(first_request), {1, 2, 3, 4});
  auto second = fixture.MakeGenerator(std::move(second_request), {1, 2, 3, 4, 5, 6});

  std::string first_text;
  std::string second_text;
  std::thread first_thread([&] {
    while (!first->IsDone()) {
      first->GenerateNextToken();
      first_text += first->Decode();
    }
  });
  std::thread second_thread([&] {
    while (!second->IsDone()) {
      second->GenerateNextToken();
      second_text += second->Decode();
    }
  });

  first_thread.join();
  second_thread.join();

  EXPECT_EQ(first_text, "10,11,");
  EXPECT_EQ(second_text, "20,21,");
  EXPECT_EQ(first->PromptTokenCount(), 4);
  EXPECT_EQ(first->TokenCount(), 6);
  EXPECT_EQ(second->PromptTokenCount(), 6);
  EXPECT_EQ(second->TokenCount(), 8);
  EXPECT_TRUE(first_backend->removed);
  EXPECT_TRUE(second_backend->removed);
}

TEST(EngineChatGeneratorTest, CancelClosesRequestAndStopsDrivingHost) {
  AdapterFixture fixture;
  auto [request, backend_request] = fixture.Submit();
  auto generator = fixture.MakeGenerator(std::move(request), {1, 2, 3});

  generator->Cancel();
  generator->Cancel();

  EXPECT_TRUE(generator->IsDone());
  EXPECT_EQ(generator->TokenCount(), 3);
  EXPECT_TRUE(backend_request->removed);
}

TEST(EngineChatGeneratorTest, CancellationAtStepBoundaryClosesWithoutDrivingAnotherStep) {
  AdapterFixture fixture;
  auto [request, backend_request] = fixture.Submit();
  fixture.backend->ready = {{.request = backend_request, .tokens = {}, .complete = false}};
  bool cancelled = false;
  auto generator = std::make_unique<EngineChatGenerator>(
      fixture.host, std::move(request), [](int32_t) { return std::string{}; },
      std::vector<int32_t>{1, 2, 3}, EngineChatGeneratorMode::kStateless,
      [&cancelled] { return std::exchange(cancelled, true); });

  generator->GenerateNextToken();

  EXPECT_TRUE(generator->IsDone());
  EXPECT_TRUE(backend_request->removed);
  EXPECT_TRUE(fixture.backend->ready.empty());
}

TEST(EngineChatGeneratorTest, ResidentContinuationTracksExactTokensAndPerTurnUsage) {
  AdapterFixture fixture;
  auto [request, backend_request] = fixture.Submit(EngineRequestMode::kResident);
  fixture.backend->ready = {{.request = backend_request, .tokens = {10, 11}, .complete = true}};
  auto generator = fixture.MakeGenerator(
      std::move(request), {1, 2}, EngineChatGeneratorMode::kResident);

  EXPECT_EQ(generator->GenerateAll(), "10,11,");
  EXPECT_TRUE(generator->IsReadyForContinuation());
  EXPECT_EQ(CopyTokens(generator->InitialTokenIds()), (std::vector<int32_t>{1, 2}));
  EXPECT_EQ(CopyTokens(generator->GeneratedTokenIds()), (std::vector<int32_t>{10, 11}));
  EXPECT_EQ(CopyTokens(generator->ResidentTokenIds()),
            (std::vector<int32_t>{1, 2, 10, 11}));

  EXPECT_TRUE(generator->TryContinue(std::vector<int32_t>{1, 2, 10, 11, 20, 21}));
  EXPECT_EQ(backend_request->continuation_tokens, (std::vector<int32_t>{20, 21}));
  EXPECT_EQ(generator->PromptTokenCount(), 6);
  EXPECT_EQ(generator->TokenCount(), 6);
  EXPECT_TRUE(generator->GeneratedTokenIds().empty());

  fixture.backend->ready = {{.request = backend_request, .tokens = {30}, .complete = true}};
  EXPECT_EQ(generator->GenerateAll(), "30,");
  EXPECT_EQ(generator->PromptTokenCount(), 6);
  EXPECT_EQ(generator->TokenCount(), 7);
  EXPECT_EQ(CopyTokens(generator->GeneratedTokenIds()), (std::vector<int32_t>{30}));
  EXPECT_FALSE(backend_request->removed);
}

TEST(EngineChatGeneratorTest, ResidentContinuationRequiresCompleteDrainedTurn) {
  AdapterFixture fixture;
  auto [request, backend_request] = fixture.Submit(EngineRequestMode::kResident);
  auto generator = fixture.MakeGenerator(
      std::move(request), {1, 2}, EngineChatGeneratorMode::kResident);

  EXPECT_FALSE(generator->IsReadyForContinuation());
  EXPECT_FALSE(generator->TryContinue(std::vector<int32_t>{1, 2, 3}));

  fixture.backend->ready = {{.request = backend_request, .tokens = {10}, .complete = true}};
  fixture.host->Step();

  EXPECT_FALSE(generator->IsReadyForContinuation());
  EXPECT_FALSE(generator->TryContinue(std::vector<int32_t>{1, 2, 10, 20}));
  EXPECT_TRUE(backend_request->continuation_tokens.empty());

  generator->GenerateNextToken();
  EXPECT_EQ(generator->Decode(), "10,");
  EXPECT_TRUE(generator->IsReadyForContinuation());
}

TEST(EngineChatGeneratorTest, ResidentPrefixMismatchNeverCallsContinue) {
  AdapterFixture fixture;
  auto [request, backend_request] = fixture.Submit(EngineRequestMode::kResident);
  fixture.backend->ready = {{.request = backend_request, .tokens = {10}, .complete = true}};
  auto generator = fixture.MakeGenerator(
      std::move(request), {1, 2}, EngineChatGeneratorMode::kResident);
  EXPECT_EQ(generator->GenerateAll(), "10,");

  EXPECT_FALSE(generator->TryContinue(std::vector<int32_t>{1, 2, 10}));
  EXPECT_FALSE(generator->TryContinue(std::vector<int32_t>{1, 9, 10, 20}));
  EXPECT_TRUE(backend_request->continuation_tokens.empty());
  EXPECT_TRUE(generator->IsReadyForContinuation());
}

TEST(EngineChatGeneratorTest, CancelPreservesBackendBufferedOutput) {
  AdapterFixture fixture;
  auto [request, backend_request] = fixture.Submit(EngineRequestMode::kResident);
  fixture.backend->ready = {{.request = backend_request, .tokens = {10, 11}, .complete = true}};
  fixture.host->Step();
  auto retained_request = request;
  auto generator = fixture.MakeGenerator(
      std::move(request), {1, 2}, EngineChatGeneratorMode::kResident);

  generator->Cancel();

  EXPECT_TRUE(backend_request->removed);
  EXPECT_EQ(retained_request->DrainGeneratedTokens(), (std::vector<int32_t>{10, 11}));
}

TEST(EngineChatGeneratorTest, ClosingAtCallerLimitCountsOnlyConsumedTokensAndPreventsReuse) {
  AdapterFixture fixture;
  auto [request, backend_request] = fixture.Submit(EngineRequestMode::kResident);
  fixture.backend->ready = {{.request = backend_request, .tokens = {10, 11}, .complete = false}};
  auto retained_request = request;
  auto generator = fixture.MakeGenerator(
      std::move(request), {1, 2}, EngineChatGeneratorMode::kResident);

  generator->GenerateNextToken();
  EXPECT_EQ(generator->Decode(), "10,");
  EXPECT_EQ(generator->PromptTokenCount(), 2);
  EXPECT_EQ(generator->TokenCount(), 3);
  EXPECT_FALSE(generator->IsReadyForContinuation());

  generator->Cancel();

  EXPECT_TRUE(generator->IsDone());
  EXPECT_TRUE(backend_request->removed);
  EXPECT_EQ(retained_request->DrainGeneratedTokens(), (std::vector<int32_t>{11}));
}

TEST(EngineChatGeneratorTest, BackendErrorClosesResidentRequest) {
  AdapterFixture fixture;
  auto [request, backend_request] = fixture.Submit(EngineRequestMode::kResident);
  fixture.backend->fail_step = true;
  auto generator = fixture.MakeGenerator(
      std::move(request), {1, 2}, EngineChatGeneratorMode::kResident);

  EXPECT_THROW(generator->GenerateNextToken(), std::runtime_error);
  EXPECT_TRUE(backend_request->removed);
  EXPECT_TRUE(generator->IsDone());
}

TEST(EngineChatGeneratorTest, ResidentDestructorClosesCompletedRequest) {
  AdapterFixture fixture;
  auto [request, backend_request] = fixture.Submit(EngineRequestMode::kResident);
  fixture.backend->ready = {{.request = backend_request, .tokens = {10}, .complete = true}};

  {
    auto generator = fixture.MakeGenerator(
        std::move(request), {1, 2}, EngineChatGeneratorMode::kResident);
    EXPECT_EQ(generator->GenerateAll(), "10,");
    EXPECT_EQ(fixture.backend->remove_count, 0);
  }

  EXPECT_EQ(fixture.backend->remove_count, 1);
}

}  // namespace
}  // namespace fl
