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

struct AdapterRequest final : EngineBackend::Request {
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
    if (ready.empty()) {
      return nullptr;
    }

    auto next = std::move(ready.front());
    ready.erase(ready.begin());
    return std::make_unique<ReadyRequest>(
        ReadyRequest{.request = next.request, .tokens = std::move(next.tokens), .is_turn_complete = next.complete});
  }

  void Continue(Request&, std::span<const int32_t>) override {}

  void Remove(Request& request) override {
    static_cast<AdapterRequest&>(request).removed = true;
  }

  std::vector<Ready> ready;
};

struct AdapterFixture {
  AdapterFixture() {
    auto owned_backend = std::make_unique<AdapterBackend>();
    backend = owned_backend.get();
    host = std::make_shared<EngineHost>(std::move(owned_backend));
  }

  std::pair<std::shared_ptr<EngineRequest>, AdapterRequest*> Submit() {
    auto prepared = std::make_unique<AdapterRequest>();
    auto* raw = prepared.get();
    return {host->SubmitPrepared(std::move(prepared)), raw};
  }

  std::unique_ptr<EngineChatGenerator> MakeGenerator(std::shared_ptr<EngineRequest> request, int prompt_tokens) {
    return std::make_unique<EngineChatGenerator>(
        host, std::move(request), [](int32_t token) { return std::to_string(token) + ","; }, prompt_tokens);
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

TEST(EngineChatGeneratorTest, SharedHostDemultiplexesBufferedTokensWithoutLoss) {
  AdapterFixture fixture;
  auto [first_request, first_backend] = fixture.Submit();
  auto [second_request, second_backend] = fixture.Submit();
  fixture.backend->ready = {
      {.request = second_backend, .tokens = {20}, .complete = false},
      {.request = first_backend, .tokens = {10, 11}, .complete = true},
      {.request = second_backend, .tokens = {21}, .complete = true},
  };

  auto first = fixture.MakeGenerator(std::move(first_request), 4);
  auto second = fixture.MakeGenerator(std::move(second_request), 6);

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
  auto generator = fixture.MakeGenerator(std::move(request), 3);

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
      fixture.host, std::move(request), [](int32_t) { return std::string{}; }, 3, [&cancelled] {
        return std::exchange(cancelled, true);
      });

  generator->GenerateNextToken();

  EXPECT_TRUE(generator->IsDone());
  EXPECT_TRUE(backend_request->removed);
  EXPECT_TRUE(fixture.backend->ready.empty());
}

}  // namespace
}  // namespace fl
