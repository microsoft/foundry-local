// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <foundry_local/foundry_local_c.h>

#include "inferencing/model_session_lease.h"
#include "inferencing/session/callback_handler.h"
#include "inferencing/session/live_session_registry.h"
#include "inferencing/session/operation_context.h"
#include "inferencing/session/operation_state.h"
#include "inferencing/session/request.h"
#include "inferencing/session/response.h"
#include "inferencing/session/types.h"
#include "model_info.h"
#include "util/key_value_pairs.h"

namespace fl {

class ILogger;
class ITelemetry;
class Model;

/// The separately allocated, shared, polymorphic body of a session.
///
/// Everything executable lives here: the model lease, the modality state, the common options and tool
/// definitions, the streaming callback, the terminal flag, the serial admission gate and the set of live
/// operations. The `Session` object callers hold is only a movable shared_ptr facade over one of these.
///
/// Why the split exists. The previous shape kept the executable state on the Session itself and made
/// ~Session wait on a reader/writer "owner gate" so an in-flight run could keep borrowing it. That is
/// undefined behaviour — the wait begins after destruction has already started and after derived members
/// have died — and it self-deadlocks whenever the thing releasing the session is the session's own streaming
/// callback. Here an Operation captures a strong shared_ptr to the runtime at CreateOperation and never
/// dereferences a Session at all, so:
///   - Session destruction is *signal-only*: cancel every operation, drop the owner reference, return. It
///     never waits and can safely run on a callback thread.
///   - Physical destruction of the runtime — and therefore of the modality state and the model lease —
///     happens once the last operation or callback reference goes away, which is by construction after every
///     user of that state has finished.
///   - There is no ownership cycle: the runtime holds operations strongly, operations hold the runtime
///     strongly only for the duration of their own lifetime, and OperationState refers back weakly.
///
/// A runtime never moves. Moving a Session moves a shared_ptr; cancellation identity, the registry entry,
/// the terminal flag and every registered operation are untouched by construction.
class SessionRuntime : public std::enable_shared_from_this<SessionRuntime> {
 public:
  using StreamingCallbackFn = std::function<int(flStreamingCallbackData, void*)>;

  virtual ~SessionRuntime() noexcept;

  SessionRuntime(const SessionRuntime&) = delete;
  SessionRuntime& operator=(const SessionRuntime&) = delete;
  SessionRuntime(SessionRuntime&&) = delete;
  SessionRuntime& operator=(SessionRuntime&&) = delete;

  /// The concrete session type (no RTTI needed).
  virtual SessionType Type() const = 0;

  /// Number of completed turns. Only meaningful for chat runtimes.
  virtual size_t TurnCount() const { return 0; }

  /// Undo the last `count` turns. Only meaningful for chat runtimes.
  /// @throws fl::Exception if the runtime does not support turn management or count > TurnCount().
  virtual void UndoTurns(size_t count);

  // --- configuration (forwarded from the facade) ---

  /// @throws fl::Exception if tool_def.json_schema is not valid JSON.
  void AddToolDefinition(ToolDefinition tool_def);

  bool RemoveToolDefinition(const std::string& tool_name) {
    auto it = std::find_if(tool_definitions_.begin(), tool_definitions_.end(),
                           [&](const ToolDefinition& td) { return td.name == tool_name; });
    if (it == tool_definitions_.end()) {
      return false;
    }

    tool_definitions_.erase(it);
    return true;
  }

  const std::vector<ToolDefinition>& ToolDefinitions() const { return tool_definitions_; }

  void ClearToolDefinitions() { tool_definitions_.clear(); }

  void SetSessionOptions(const KeyValuePairs& options) {
    session_options_ = options;
    SetSessionOptionsImpl(session_options_);
  }

  void SetStreamingCallback(StreamingCallbackFn callback, void* user_data) {
    callback_fn_ = std::move(callback);
    callback_user_data_ = user_data;
  }

  // --- operation registry and terminal state ---

  /// True once CancelAll()/MarkTerminal() has made this session terminal. Future operations are rejected.
  bool IsTerminal() const;

  /// Reject creation while terminal, otherwise register `state` strongly. Returns false if terminal (the
  /// caller must reject with FOUNDRY_LOCAL_ERROR_INVALID_USAGE).
  bool RegisterOperation(const std::shared_ptr<OperationState>& state);

  /// Drop the strong ref to `state`. Safe to call for an operation that was never registered.
  void DeregisterOperation(const OperationState& state) noexcept;

  /// Terminal, irreversible, signal-only. Marks the session terminal, wakes the admission gate, and stops
  /// every registered operation. Never waits for operations, callbacks, or anything else.
  void CancelAll() noexcept;

  /// Force the session terminal without enumerating operations. Used by the registry when a runtime is
  /// registered while shutdown admission is closed, so its future operations are rejected.
  void MarkTerminal() noexcept;

  // --- serial admission gate (chat/audio: one run at a time, but interruptibly) ---

  /// Wait for the serial permit. Wakes for: permit available, this operation stopped, the operation's
  /// deadline, or terminal session cancel. Latches the stop itself (outside the gate lock) so the caller
  /// only has to branch on the returned decision:
  ///   kCompleted -> permit granted, caller should run.
  ///   anything else -> caller must not run; the outcome is already latched on the state.
  OperationOutcome AcquireSerialPermit(OperationState& state);

  /// Release the serial permit taken by AcquireSerialPermit and wake the next waiter.
  void ReleaseSerialPermit();

  /// Wake all admission waiters (used when an operation is stopped so a queued run re-evaluates).
  void NotifyAdmission() noexcept;

  // --- execution ---

  /// Orchestrate one operation run: serial admission gate, deadline watchdog, ProcessRequestImpl, finalize.
  /// Called only by Operation::Process, which holds this runtime alive for the whole call. `request` is the
  /// operation's immutable snapshot — never the caller's live Request.
  OperationOutcome Run(const std::shared_ptr<OperationState>& state, const Request& request, Response& response);

 protected:
  SessionRuntime(const fl::Model& catalog_model, ILogger& logger, ITelemetry& telemetry, ModelSessionLease lease,
                 bool allow_concurrent_requests = false);

  const std::string& CatalogModelId() const { return catalog_model_id_; }

  const ModelInfo& CatalogModelInfo() const { return catalog_model_info_; }

  ILogger& Logger() { return logger_; }

  /// The leased model. Only valid on a runtime constructed with an engaged lease.
  GenAIModelInstance& Model() const { return model_lease_.Model(); }

  virtual void SetSessionOptionsImpl(const KeyValuePairs& /*options*/) {}

  /// Merge session-level options with per-request options.
  /// Returns a copy of session options with request options overlaid (request wins on conflict).
  KeyValuePairs MergedOptions(const KeyValuePairs& request_options) const {
    if (request_options.empty()) {
      return session_options_;
    }

    KeyValuePairs merged = session_options_;
    for (const auto& [key, value] : request_options) {
      merged.Add(key, value);
    }

    return merged;
  }

  /// Derived runtimes implement the actual generation logic.
  ///
  /// `operation` is the explicit, per-run stop authority: generation loops poll operation.ShouldStop(),
  /// generator publication goes through ActiveGenerator(operation, ...), and the outcome is committed only
  /// after operation.TrySeal() succeeds. It must be threaded through every helper that polls cancellation,
  /// registers a generator or commits state — pure input-parsing helpers stay Request-only.
  virtual void ProcessRequestImpl(const OperationContext& operation, const Request& request,
                                  Response& response) = 0;

  /// Create a per-request callback handler bound to this operation. Returns nullptr if no callback is set.
  /// The handler is owned by the caller (unique_ptr) and drains+joins on destruction, which must happen
  /// before `operation` goes out of scope — guaranteed because handlers are locals of ProcessRequestImpl.
  std::unique_ptr<CallbackHandler> CreateCallbackHandler(const OperationContext& operation) {
    if (!callback_fn_) {
      return nullptr;
    }

    return std::make_unique<CallbackHandler>(operation, callback_fn_, logger_, callback_user_data_);
  }

  /// Called once the request has finished and its deadline watchdog has joined. Derived runtimes use it to
  /// drop state a timeout or an engine-delivered cancellation invalidated after the watchdog can no longer
  /// reach the generator. `operation` is the finishing operation — passed explicitly, never inferred.
  virtual void OnRequestFinished(const OperationContext& /*operation*/, const Request& /*request*/) noexcept {}

  const KeyValuePairs& SessionOptions() const { return session_options_; }

 private:
  /// Catalog metadata is copied at construction. The catalog may refresh or be destroyed independently once
  /// the runtime has its model lease, so no operation may retain a borrowed Model reference.
  std::string catalog_model_id_;
  ModelInfo catalog_model_info_;

  /// Runtime services follow the Manager lifetime contract: production callers retain the Manager until all
  /// sessions/operations are released, while direct internal constructors must keep their supplied services
  /// alive. Unlike the old design, no Session or Request object is borrowed by an operation.
  ILogger& logger_;
  ITelemetry& telemetry_;

  /// The model lease lives on the base, which is destroyed *after* every derived member. That ordering is
  /// load-bearing: a derived runtime's cached generators and OGA state die first, and only then is the lease
  /// released, so the model can never be unloaded while something derived is still using it.
  ModelSessionLease model_lease_;

  std::vector<ToolDefinition> tool_definitions_;
  KeyValuePairs session_options_;
  StreamingCallbackFn callback_fn_;
  void* callback_user_data_ = nullptr;
  const bool allow_concurrent_requests_;

  mutable std::mutex mu_;
  std::condition_variable admission_cv_;
  bool terminal_ = false;
  bool serial_busy_ = false;
  std::vector<std::shared_ptr<OperationState>> operations_;
};

/// Create a runtime and publish it to the process-wide live registry in one step.
///
/// Registration cannot happen inside the constructor (shared_from_this is not yet usable there) and must
/// not be left to callers, because a runtime that is invisible to the registry would survive a shutdown
/// cancel and pin its model. The registry holds the runtime weakly; the facade holds the strong reference.
template <typename Runtime, typename... Args>
std::shared_ptr<Runtime> MakeSessionRuntime(Args&&... args) {
  auto runtime = std::make_shared<Runtime>(std::forward<Args>(args)...);
  LiveSessionRegistry::Instance().Add(runtime);
  return runtime;
}

}  // namespace fl
