// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <foundry_local/foundry_local_c.h>

#include "inferencing/session/callback_handler.h"
#include "inferencing/session/request.h"
#include "inferencing/session/response.h"
#include "inferencing/session/types.h"
#include "telemetry/invocation_context.h"
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

  Session(Session&&) = default;
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
  void ProcessRequest(const Request& request, Response& response);

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

  /// Telemetry context for the next ProcessRequest. HTTP handlers set this to an
  /// indirect child of the route's context so the kSessionProcessRequest action
  /// and the per-inference Model event are marked indirect and share the route's
  /// correlation id. When unset (direct SDK use), ProcessRequest mints its own
  /// direct context per call.
  void SetRequestContext(InvocationContext context) {
    request_context_ = std::move(context);
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

  /// Execution provider the loaded model runs on (e.g. "CPU", "CUDA"). Surfaced
  /// on the per-inference Model telemetry event. Empty when not known.
  virtual std::string ExecutionProvider() const { return {}; }

  /// Create a per-request callback handler. Returns nullptr if no callback is set.
  /// The handler is owned by the caller (unique_ptr) and drains+joins on destruction.
  std::unique_ptr<CallbackHandler> CreateCallbackHandler(const Request& request) {
    if (!callback_fn_) {
      return nullptr;
    }

    return std::make_unique<CallbackHandler>(request, callback_fn_, logger_, callback_user_data_);
  }

  const KeyValuePairs& SessionOptions() const { return session_options_; }

 private:
  const fl::Model& catalog_model_;
  ILogger& logger_;
  ITelemetry& telemetry_;
  std::vector<ToolDefinition> tool_definitions_;
  KeyValuePairs session_options_;
  StreamingCallbackFn callback_fn_;
  void* callback_user_data_ = nullptr;
  std::optional<InvocationContext> request_context_;
  const bool allow_concurrent_requests_;
  mutable std::unique_ptr<std::mutex> request_mutex_ = std::make_unique<std::mutex>();
};

}  // namespace fl
