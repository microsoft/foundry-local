// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/engine/engine_backend.h"

#include <ort_genai.h>

#include <utility>

namespace fl {
namespace {

std::unique_ptr<OgaSequences> MakeSequences(std::span<const int32_t> tokens) {
  auto sequences = OgaSequences::Create();
  sequences->Append(tokens.data(), tokens.size());
  return sequences;
}

class OgaEngineRequest final : public EngineBackend::Request {
 public:
  explicit OgaEngineRequest(std::unique_ptr<OgaRequest> request) : request_(std::move(request)) {
    request_->SetOpaqueData(static_cast<EngineBackend::Request*>(this));
  }

  OgaRequest& Get() {
    return *request_;
  }

 private:
  std::unique_ptr<OgaRequest> request_;
};

class OgaEngineBackend final : public EngineBackend {
 public:
  explicit OgaEngineBackend(OgaModel& model) : engine_(OgaEngine::Create(model)) {}

  std::unique_ptr<Request> CreateRequest(OgaGeneratorParams& params,
                                         std::span<const int32_t> tokens) override {
    auto request = std::make_unique<OgaEngineRequest>(OgaRequest::Create(params));
    auto sequences = MakeSequences(tokens);
    request->Get().AddTokens(*sequences);
    return request;
  }

  void Add(Request& request) override {
    engine_->Add(AsOgaRequest(request).Get());
  }

  std::unique_ptr<ReadyRequest> Step() override {
    // Every non-null handle returned by OgaEngine::Step must be destroyed even though the engine retains the request.
    auto ready_handle = engine_->Step();
    if (!ready_handle) {
      return nullptr;
    }

    auto* request = static_cast<Request*>(ready_handle->GetOpaqueData());
    std::vector<int32_t> tokens;
    while (ready_handle->HasUnseenTokens()) {
      tokens.push_back(ready_handle->GetUnseenToken());
    }

    const auto is_turn_complete = ready_handle->IsTurnComplete();
    // ready_handle is destroyed before this function returns, so Continue cannot race an undrained ready notification.
    return std::make_unique<ReadyRequest>(
        ReadyRequest{.request = request, .tokens = std::move(tokens), .is_turn_complete = is_turn_complete});
  }

  void Continue(Request& request, std::span<const int32_t> tokens) override {
    auto sequences = MakeSequences(tokens);
    AsOgaRequest(request).Get().Continue(*sequences);
  }

  void Remove(Request& request) override {
    engine_->Remove(AsOgaRequest(request).Get());
  }

 private:
  static OgaEngineRequest& AsOgaRequest(Request& request) {
    return static_cast<OgaEngineRequest&>(request);
  }

  std::unique_ptr<OgaEngine> engine_;
};

}  // namespace

std::unique_ptr<EngineBackend> CreateOgaEngineBackend(OgaModel& model) {
  return std::make_unique<OgaEngineBackend>(model);
}

}  // namespace fl
