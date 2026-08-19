// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/engine/engine_backend.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

struct OgaGeneratorParams;
struct OgaModel;

namespace fl {

class EngineHostState;

enum class EngineRequestMode {
  /// Release engine-owned KV state as soon as the generation turn completes.
  kStateless,
  /// Retain engine-owned KV state after turn completion so the caller can explicitly Continue or Close.
  kResident,
};

/// Opaque logical request hosted by one shared EngineHost.
///
/// All methods are serialized with EngineHost::Step. Close is cancellation at that serialization boundary; it does not
/// interrupt a Step already executing on another thread. Buffered output remains drainable after Close.
class EngineRequest final {
 public:
  EngineRequest(const EngineRequest&) = delete;
  EngineRequest& operator=(const EngineRequest&) = delete;

  std::vector<int32_t> DrainGeneratedTokens();
  bool HasGeneratedTokens() const;
  std::optional<int32_t> PopGeneratedToken();
  bool IsTurnComplete() const;
  bool IsClosed() const noexcept;
  /// Begin another turn after completion. All generated output must be drained first.
  void Continue(std::span<const int32_t> tokens);
  void Close();

 private:
  friend class EngineHostState;

  EngineRequest(std::weak_ptr<EngineHostState> host,
                std::unique_ptr<EngineBackend::Request> backend_request,
                EngineRequestMode mode);

  std::weak_ptr<EngineHostState> host_;
  std::unique_ptr<EngineBackend::Request> backend_request_;
  std::vector<int32_t> unread_tokens_;
  EngineRequestMode mode_;
  bool is_turn_complete_{false};
  std::atomic<bool> is_closed_{false};
};

/// Serialized owner of one ORT GenAI Engine and all requests submitted to it.
///
/// The OgaModel used by Create must outlive this host. Call Shutdown for deterministic removal before releasing the
/// model. Destruction performs best-effort no-throw cleanup; only explicit Shutdown can report cleanup failures.
class EngineHost final {
 public:
  static std::shared_ptr<EngineHost> Create(OgaModel& model);

  /// Backend injection is internal infrastructure for model-free tests.
  explicit EngineHost(std::unique_ptr<EngineBackend> backend);
  ~EngineHost();

  EngineHost(const EngineHost&) = delete;
  EngineHost& operator=(const EngineHost&) = delete;

  /// Submits a stateless request by default. Resident mode is an explicit opt-in for a later Continue call.
  std::shared_ptr<EngineRequest> Submit(OgaGeneratorParams& params,
                                        std::span<const int32_t> tokens,
                                        EngineRequestMode mode = EngineRequestMode::kStateless);

  /// Adds a backend request whose initial tokens have already been installed. Used by narrow backend test doubles.
  std::shared_ptr<EngineRequest> SubmitPrepared(
      std::unique_ptr<EngineBackend::Request> request,
      EngineRequestMode mode = EngineRequestMode::kStateless);

  /// Performs one shared-engine step and returns the corresponding Foundry request, or nullptr when none is ready.
  std::shared_ptr<EngineRequest> Step();

  /// Removes every open request at a serialized boundary and releases the engine.
  void Shutdown();

 private:
  std::shared_ptr<EngineHostState> state_;
};

}  // namespace fl
