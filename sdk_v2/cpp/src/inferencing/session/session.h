// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <foundry_local/foundry_local_c.h>

#include "inferencing/session/request.h"
#include "inferencing/session/response.h"
#include "inferencing/session/session_runtime.h"
#include "inferencing/session/types.h"
#include "util/key_value_pairs.h"

namespace fl {

class Model;      // forward declaration
class Operation;  // forward declaration — per-invocation unit of work created by CreateOperation

/// Defined in operation.h. Controls whether a stopped run's partially-built response is cleared
/// (kExplicitAtomic, the deferred Operation API) or preserved (kLegacyPartial, ProcessRequest).
enum class ResponseVisibility : uint8_t;

/// Handle to a model inference session.
///
/// A Session is a **facade**: it owns nothing but a `shared_ptr<SessionRuntime>` and forwards. All
/// executable state — model lease, modality state, options, tool definitions, callback, terminal flag,
/// admission gate, live operations — lives in the runtime, which is separately allocated and shared.
///
/// What that buys, and why the previous design could not:
///   - **Moves are trivial and safe.** Moving a Session moves a shared_ptr. There is no half-moved object to
///     protect, so PrepareMove/FinishMove, the executor gate and the `owns_session_` refcount-ownership
///     flags are all gone.
///   - **Destruction never waits.** `~Session` signals CancelAll() and drops its reference. It does not
///     block on in-flight work, which is what makes it safe to release a Session from inside its own
///     streaming callback — the previous destructor-owner-lock scheme deadlocked there, and was undefined
///     behaviour anyway because the wait started after destruction had begun.
///   - **Execution never dereferences a Session.** An Operation captures the runtime strongly at
///     CreateOperation, so the runtime (and the model lease inside it) outlives the facade exactly as long
///     as some operation or callback still needs it, and is destroyed immediately afterwards.
///
/// Derived facades (ChatSession, AudioSession, EmbeddingsSession) add construction and typed accessors only;
/// they hold no state of their own and need no destructor or move-constructor ceremony.
class Session {
 public:
  using StreamingCallbackFn = SessionRuntime::StreamingCallbackFn;

  explicit Session(std::shared_ptr<SessionRuntime> runtime) : runtime_(std::move(runtime)) {}

  /// Signal-only: cancels every operation of this session and drops the owner reference. Never waits.
  virtual ~Session() noexcept;

  Session(Session&&) noexcept = default;
  Session& operator=(Session&&) = delete;

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  /// Factory: creates the correct derived Session for the model's task type, holding a model lease acquired
  /// atomically against unload.
  static std::unique_ptr<Session> Create(const Model& model);

  /// Returns the concrete session type (no RTTI needed).
  SessionType Type() const { return CheckedRuntime().Type(); }

  /// Create a single-use operation for `request`, synchronously and before any worker scheduling.
  ///
  /// Snapshots the request's timeout into an absolute steady-clock deadline immediately (so the budget
  /// includes any serialized queue wait), claims the Request (rejecting concurrent use of the same Request
  /// with FOUNDRY_LOCAL_ERROR_INVALID_USAGE), captures an immutable RequestSnapshot of the execution data,
  /// rejects a terminal or moved-from session with the same error, and registers a strong per-operation
  /// state with the runtime. A failure anywhere after the claim rolls the claim and the registration back.
  ///
  /// @throws fl::Exception (FOUNDRY_LOCAL_ERROR_INVALID_USAGE) if the session is terminal or moved-from, or
  ///         the Request is already in use by another operation.
  std::unique_ptr<Operation> CreateOperation(const Request& request);

  /// Convenience create -> process. Overlays session parameters (in the runtime), runs the operation, and
  /// waits for all async streaming callbacks to complete before returning.
  ///
  /// Outcome contract (unchanged legacy behaviour):
  ///   - Natural stop -> returns normally.
  ///   - Any cancellation that is not a deadline — an explicit Session/Operation/Request cancel, or a
  ///     streaming callback asking to stop -> returns normally with
  ///     response.finish_reason == FOUNDRY_LOCAL_FINISH_NONE.
  ///   - A deadline expiry -> fl::Exception(FOUNDRY_LOCAL_ERROR_TIMEOUT).
  ///   - Submitting on an already-terminal session -> fl::Exception(FOUNDRY_LOCAL_ERROR_INVALID_USAGE).
  ///   - A genuine inference error -> the original exception, unchanged.
  ///
  /// Note the deliberate asymmetry with Operation::Process(), which reports kCancelled so the explicit
  /// operation API can map it to FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED.
  void ProcessRequest(const Request& request, Response& response);

  /// Terminal, irreversible session cancel. Signals every created/running operation to stop and rejects
  /// future CreateOperation calls. Safe to call from any thread; idempotent; signal-only.
  void Cancel();

  /// Add a tool definition to this session.
  /// @throws fl::Exception if tool_def.json_schema is not valid JSON.
  void AddToolDefinition(ToolDefinition tool_def) { CheckedRuntime().AddToolDefinition(std::move(tool_def)); }

  /// Remove a previously-added tool definition by name. Returns true if one was found and removed.
  bool RemoveToolDefinition(const std::string& tool_name) {
    return CheckedRuntime().RemoveToolDefinition(tool_name);
  }

  /// Get the tool definitions added to this session.
  const std::vector<ToolDefinition>& ToolDefinitions() const { return CheckedRuntime().ToolDefinitions(); }

  /// Remove all tool definitions from this session. Needed when a session is reused across requests (e.g.
  /// via Responses API `previous_response_id`) and the new request brings its own tools — the
  /// request-is-self-contained model means stale tools must not leak across turns.
  void ClearToolDefinitions() { CheckedRuntime().ClearToolDefinitions(); }

  /// Get the number of completed turns. Only meaningful for chat sessions.
  size_t TurnCount() const { return CheckedRuntime().TurnCount(); }

  /// Undo the last `count` turns. Only meaningful for chat sessions.
  /// @throws fl::Exception if the session type does not support turn management or count > TurnCount().
  void UndoTurns(size_t count) { CheckedRuntime().UndoTurns(count); }

  /// Session-level parameters overlaid onto each request.
  void SetSessionOptions(const KeyValuePairs& options) { CheckedRuntime().SetSessionOptions(options); }

  void SetStreamingCallback(StreamingCallbackFn callback, void* user_data = nullptr) {
    CheckedRuntime().SetStreamingCallback(std::move(callback), user_data);
  }

  /// The stable, shared runtime. Exposed for the ABI layer and for SessionManager, which keys registration
  /// on runtime identity rather than on a facade address that can move. Returned by value so the caller
  /// immediately owns a lifetime pin; null on a moved-from Session.
  std::shared_ptr<SessionRuntime> Runtime() const { return runtime_; }

 protected:
  /// The runtime, validated to be non-null. Used by the typed facades for their static_cast.
  SessionRuntime& RuntimeRef() const { return CheckedRuntime(); }

 private:
  /// Shared implementation for the explicit deferred API and the synchronous convenience path.
  /// Deferred operations reject non-clonable borrowed items; ProcessRequest may retain them because it does
  /// not return until processing and callback drain have completed.
  std::unique_ptr<Operation> CreateOperationImpl(const Request& request, bool allow_borrowed_items,
                                                 ResponseVisibility visibility);

  /// Access the runtime or reject use of a moved-from facade without dereferencing a null shared_ptr.
  SessionRuntime& CheckedRuntime() const;

  std::shared_ptr<SessionRuntime> runtime_;
};

}  // namespace fl
