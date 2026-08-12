// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <foundry_local/foundry_local_c.h>

#include "inferencing/session/callback_handler.h"
#include "inferencing/session/cancellable.h"
#include "inferencing/session/request.h"
#include "inferencing/session/response.h"
#include "inferencing/session/types.h"
#include "util/key_value_pairs.h"

namespace fl {

class ILogger;     // forward declaration
class ITelemetry;  // forward declaration
class Model;       // forward declaration

/// Base class for model inference sessions.
/// Manages lifecycle, request dispatch, streaming callbacks, and tool definitions.
/// Derived classes hold a reference to their specific loaded model type.
///
/// Derived classes specialize for different inference patterns:
///   - ChatSession: conversational text generation with message history
///   - Future: predictive inference, realtime audio, multi-modal
class Session {
 public:
  virtual ~Session();

  Session(Session&&) noexcept;
  Session& operator=(Session&&) = delete;

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  /// Factory: creates the correct derived Session for the model's task type.
  static std::unique_ptr<Session> Create(const Model& model);

  /// Returns the concrete session type (no RTTI needed).
  virtual SessionType Type() const = 0;

  /// Process a request: overlays session parameters, delegates to the derived
  /// class, then waits for all async streaming callbacks to complete.
  /// Waiting here keeps the Request reference valid for the lifetime of any
  /// in-flight callbacks and ensures the Response is fully populated on return.
  ///
  /// Cancellation applies to every path, streaming or not:
  ///   - `Session::Cancel()` from another thread stops the run promptly.
  ///   - `Request::SetTimeout()` bounds the run with a wall-clock deadline.
  /// Both work by flagging the Request and calling Cancel() on the active generator,
  /// so an in-flight ORT GenAI compute is interrupted rather than waited out.
  void ProcessRequest(const Request& request, Response& response);

  /// Signal the in-flight request (if any) to stop, and cause the next ProcessRequest
  /// on this session to abort immediately. Safe to call from any thread; idempotent.
  ///
  /// This is the teardown hook: it guarantees a runaway non-streaming generation
  /// releases the session's model refcount instead of pinning the model loaded.
  void Cancel();

  /// Add a tool definition to this session.
  /// @throws fl::Exception if tool_def.json_schema is not valid JSON.
  void AddToolDefinition(ToolDefinition tool_def);

  /// Remove a previously-added tool definition by name.
  /// Returns true if a matching tool was found and removed, false otherwise.
  bool RemoveToolDefinition(const std::string& tool_name) {
    auto it = std::find_if(tool_definitions_.begin(), tool_definitions_.end(),
                           [&](const ToolDefinition& td) { return td.name == tool_name; });
    if (it == tool_definitions_.end()) {
      return false;
    }

    tool_definitions_.erase(it);
    return true;
  }

  /// Get the tool definitions added to this session.
  const std::vector<ToolDefinition>& ToolDefinitions() const {
    return tool_definitions_;
  }

  /// Remove all tool definitions from this session. Needed when a session is reused across
  /// requests (e.g. via Responses API `previous_response_id`) and the new request brings its
  /// own tools — the request-is-self-contained model means stale tools must not leak across turns.
  void ClearToolDefinitions() {
    tool_definitions_.clear();
  }

  /// Get the number of completed turns. Only meaningful for chat sessions.
  virtual size_t TurnCount() const { return 0; }

  /// Undo the last `count` turns. Only meaningful for chat sessions.
  /// @throws fl::Exception if the session type does not support turn management or count > TurnCount().
  virtual void UndoTurns(size_t count);

  /// Session-level parameters overlaid onto each request.
  void SetSessionOptions(const KeyValuePairs& options) {
    session_options_ = options;
    SetSessionOptionsImpl(session_options_);
  }

  using StreamingCallbackFn = std::function<int(flStreamingCallbackData, void*)>;
  void SetStreamingCallback(StreamingCallbackFn callback, void* user_data = nullptr) {
    callback_fn_ = std::move(callback);
    callback_user_data_ = user_data;
  }

 protected:
  Session(const fl::Model& catalog_model, ILogger& logger, ITelemetry& telemetry,
          bool allow_concurrent_requests = false);

  const fl::Model& CatalogModel() const { return catalog_model_; }

  ILogger& Logger() { return logger_; }

  virtual void SetSessionOptionsImpl(const KeyValuePairs& /*options*/) {}

  /// Merge session-level options with per-request options.
  /// Returns a copy of session options with request options overlaid (request wins on conflict).
  /// Derived classes call this when they want a single resolved option set.
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

  /// Derived classes implement the actual generation logic.
  /// `on_token` is the resolved streaming callback (may be empty).
  /// Requests are serialized if the derived class does not opt into concurrency via allow_concurrent_requests_.
  virtual void ProcessRequestImpl(const Request& request, Response& response) = 0;

  /// Create a per-request callback handler. Returns nullptr if no callback is set.
  /// The handler is owned by the caller (unique_ptr) and drains+joins on destruction.
  std::unique_ptr<CallbackHandler> CreateCallbackHandler(const Request& request) {
    if (!callback_fn_) {
      return nullptr;
    }

    return std::make_unique<CallbackHandler>(request, callback_fn_, logger_, callback_user_data_);
  }

  const KeyValuePairs& SessionOptions() const { return session_options_; }

  /// RAII guard publishing the generator currently driving a request so that
  /// Session::Cancel() and the deadline watchdog can interrupt it mid-compute.
  ///
  /// Derived classes create one for the scope in which a generator is live. If
  /// cancellation was already requested when the guard is constructed, the
  /// generator is cancelled immediately — this closes the race where Cancel()
  /// lands between the pre-flight check and the generator becoming visible.
  ///
  /// Multiple guards may be live at once: sessions that opt into concurrency
  /// (embeddings) run several requests in parallel, and a nested scope can publish
  /// a second generator. All registered generators are cancelled together.
  class ActiveGenerator {
   public:
    ActiveGenerator(Session& session, ICancellable& generator)
        : session_(session), generator_(generator) {
      session_.AddActiveGenerator(&generator_);
    }

    ~ActiveGenerator() { session_.RemoveActiveGenerator(&generator_); }

    ActiveGenerator(const ActiveGenerator&) = delete;
    ActiveGenerator& operator=(const ActiveGenerator&) = delete;

   private:
    Session& session_;
    ICancellable& generator_;
  };

 private:
  /// Publish a generator driving a request. Cancels it inline if a stop was already
  /// requested, so a generator created after Cancel() cannot run unbounded.
  void AddActiveGenerator(ICancellable* generator);

  /// Withdraw a generator once its scope ends.
  void RemoveActiveGenerator(ICancellable* generator);

  /// Body of the deadline watchdog thread. Sleeps until the request's deadline and
  /// then interrupts the run, unless woken earlier because the request completed.
  void WatchDeadline(const Request& request);

  const fl::Model& catalog_model_;
  ILogger& logger_;
  ITelemetry& telemetry_;
  std::vector<ToolDefinition> tool_definitions_;
  KeyValuePairs session_options_;
  StreamingCallbackFn callback_fn_;
  void* callback_user_data_ = nullptr;
  const bool allow_concurrent_requests_;
  mutable std::unique_ptr<std::mutex> request_mutex_ = std::make_unique<std::mutex>();

  /// Guards active_generator_/cancel_requested_ and pairs with the condition variable for
  /// the watchdog. Held behind a unique_ptr because Session must remain movable (the
  /// Responses API caches ChatSessions by move) and mutex/condition_variable are not.
  /// Separate from request_mutex_: Cancel() must be serviceable while a request holds that lock.
  struct CancelState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<ICancellable*> active_generators;
    /// In-flight requests, so Cancel() can latch the flag that drives finish_reason and
    /// history rollback. Tracked alongside generators because a request may be cancelled
    /// before it publishes one (e.g. during prefill).
    std::vector<const Request*> active_requests_list;
    bool cancel_requested = false;
    /// Number of requests currently running, so the watchdog knows when to stop waiting.
    /// A count rather than a flag because concurrent sessions overlap requests.
    int active_requests = 0;
  };

  std::unique_ptr<CancelState> cancel_state_ = std::make_unique<CancelState>();
};

}  // namespace fl
