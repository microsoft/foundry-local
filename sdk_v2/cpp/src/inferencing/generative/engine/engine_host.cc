// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/engine/engine_host.h"
#include "exception.h"

#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace fl {

class EngineHostState final : public std::enable_shared_from_this<EngineHostState> {
 public:
  explicit EngineHostState(std::unique_ptr<EngineBackend> backend) : backend_(std::move(backend)) {
    if (!backend_) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Engine backend must not be null");
    }
  }

  std::shared_ptr<EngineRequest> Submit(OgaGeneratorParams& params,
                                        std::span<const int32_t> tokens,
                                        EngineRequestMode mode) {
    ValidateTokens(tokens, "Initial request tokens must not be empty");

    std::lock_guard<std::mutex> lock(mutex_);
    EnsureRunning();
    return SubmitLocked(backend_->CreateRequest(params, tokens), mode);
  }

  std::shared_ptr<EngineRequest> SubmitPrepared(std::unique_ptr<EngineBackend::Request> request,
                                                EngineRequestMode mode) {
    if (!request) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Prepared engine request must not be null");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    EnsureRunning();
    return SubmitLocked(std::move(request), mode);
  }

  std::shared_ptr<EngineRequest> Step() {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureRunning();

    auto ready = backend_->Step();
    if (!ready) {
      return nullptr;
    }

    const auto request_it = requests_.find(ready->request);
    if (request_it == requests_.end()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Engine returned an unknown request");
    }

    auto request = request_it->second;
    request->unread_tokens_.insert(request->unread_tokens_.end(), ready->tokens.begin(), ready->tokens.end());
    request->is_turn_complete_ = ready->is_turn_complete;

    // The backend has already drained and destroyed the ready handle. Stateless consumers release KV immediately;
    // resident consumers must explicitly Continue or Close.
    if (request->is_turn_complete_ && request->mode_ == EngineRequestMode::kStateless) {
      RemoveLocked(*request);
    }

    return request;
  }

  std::vector<int32_t> Drain(EngineRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureOwned(request);

    auto tokens = std::move(request.unread_tokens_);
    request.unread_tokens_.clear();
    return tokens;
  }

  bool HasGeneratedTokens(const EngineRequest& request) const {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureOwned(request);
    return !request.unread_tokens_.empty();
  }

  std::optional<int32_t> PopGeneratedToken(EngineRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureOwned(request);
    if (request.unread_tokens_.empty()) {
      return std::nullopt;
    }

    const auto token = request.unread_tokens_.front();
    request.unread_tokens_.erase(request.unread_tokens_.begin());
    return token;
  }

  bool IsTurnComplete(const EngineRequest& request) const {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureOwned(request);
    return request.is_turn_complete_;
  }

  void Continue(EngineRequest& request, std::span<const int32_t> tokens) {
    ValidateTokens(tokens, "Continuation tokens must not be empty");

    std::lock_guard<std::mutex> lock(mutex_);
    EnsureOpen(request);
    if (!request.is_turn_complete_) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Engine request cannot continue before its turn completes");
    }

    if (!request.unread_tokens_.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
               "Engine request output must be drained before continuing");
    }

    backend_->Continue(*request.backend_request_, tokens);
    request.is_turn_complete_ = false;
  }

  void Close(EngineRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (request.is_closed_) {
      return;
    }

    EnsureRunning();
    EnsureOwned(request);
    RemoveLocked(request);
  }

  void Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (is_shutdown_) {
      return;
    }

    for (const auto& [unused, request] : requests_) {
      static_cast<void>(unused);
      if (!request->is_closed_) {
        backend_->Remove(*request->backend_request_);
        request->is_closed_ = true;
      }
    }

    requests_.clear();
    backend_.reset();
    is_shutdown_ = true;
  }

 private:
  static void ValidateTokens(std::span<const int32_t> tokens, const char* message) {
    if (tokens.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, message);
    }
  }

  std::shared_ptr<EngineRequest> SubmitLocked(std::unique_ptr<EngineBackend::Request> backend_request,
                                              EngineRequestMode mode) {
    auto* identity = backend_request.get();
    auto request = std::shared_ptr<EngineRequest>(
        new EngineRequest(weak_from_this(), std::move(backend_request), mode));
    requests_.emplace(identity, request);

    try {
      backend_->Add(*request->backend_request_);
    } catch (...) {
      requests_.erase(identity);
      throw;
    }

    return request;
  }

  void RemoveLocked(EngineRequest& request) {
    backend_->Remove(*request.backend_request_);
    request.is_closed_ = true;
    requests_.erase(request.backend_request_.get());
  }

  void EnsureRunning() const {
    if (is_shutdown_) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Engine host is shut down");
    }
  }

  void EnsureOwned(const EngineRequest& request) const {
    if (request.is_closed_) {
      return;
    }

    EnsureRunning();
    if (!requests_.contains(request.backend_request_.get())) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Engine request does not belong to this host");
    }
  }

  void EnsureOpen(const EngineRequest& request) const {
    EnsureRunning();
    if (request.is_closed_) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Engine request is closed");
    }

    EnsureOwned(request);
  }

  // Requests are destroyed before the backend so their external ORT handles cannot outlive the engine.
  std::unique_ptr<EngineBackend> backend_;
  std::unordered_map<EngineBackend::Request*, std::shared_ptr<EngineRequest>> requests_;
  mutable std::mutex mutex_;
  bool is_shutdown_{false};
};

EngineRequest::EngineRequest(std::weak_ptr<EngineHostState> host,
                             std::unique_ptr<EngineBackend::Request> backend_request,
                             EngineRequestMode mode)
    : host_(std::move(host)), backend_request_(std::move(backend_request)), mode_(mode) {}

std::vector<int32_t> EngineRequest::DrainGeneratedTokens() {
  auto host = host_.lock();
  if (!host) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Engine host is no longer available");
  }

  return host->Drain(*this);
}

bool EngineRequest::IsTurnComplete() const {
  auto host = host_.lock();
  if (!host) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Engine host is no longer available");
  }

  return host->IsTurnComplete(*this);
}

bool EngineRequest::HasGeneratedTokens() const {
  auto host = host_.lock();
  if (!host) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Engine host is no longer available");
  }

  return host->HasGeneratedTokens(*this);
}

std::optional<int32_t> EngineRequest::PopGeneratedToken() {
  auto host = host_.lock();
  if (!host) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Engine host is no longer available");
  }

  return host->PopGeneratedToken(*this);
}

bool EngineRequest::IsClosed() const noexcept {
  return is_closed_;
}

void EngineRequest::Continue(std::span<const int32_t> tokens) {
  auto host = host_.lock();
  if (!host) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Engine host is no longer available");
  }

  host->Continue(*this, tokens);
}

void EngineRequest::Close() {
  auto host = host_.lock();
  if (!host) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Engine host is no longer available");
  }

  host->Close(*this);
}

std::shared_ptr<EngineHost> EngineHost::Create(OgaModel& model) {
  return std::make_shared<EngineHost>(CreateOgaEngineBackend(model));
}

EngineHost::EngineHost(std::unique_ptr<EngineBackend> backend)
    : state_(std::make_shared<EngineHostState>(std::move(backend))) {}

EngineHost::~EngineHost() {
  try {
    Shutdown();
  } catch (...) {
    // Explicit model unload is the error-reporting path. Destruction is only a no-throw safety net.
  }
}

std::shared_ptr<EngineRequest> EngineHost::Submit(OgaGeneratorParams& params,
                                                  std::span<const int32_t> tokens,
                                                  EngineRequestMode mode) {
  return state_->Submit(params, tokens, mode);
}

std::shared_ptr<EngineRequest> EngineHost::SubmitPrepared(std::unique_ptr<EngineBackend::Request> request,
                                                          EngineRequestMode mode) {
  return state_->SubmitPrepared(std::move(request), mode);
}

std::shared_ptr<EngineRequest> EngineHost::Step() {
  return state_->Step();
}

void EngineHost::Shutdown() {
  state_->Shutdown();
}

}  // namespace fl
