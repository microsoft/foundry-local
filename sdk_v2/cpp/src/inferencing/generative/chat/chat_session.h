// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/search_options.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/generative/toolcalling/tool_call_utils.h"
#include "inferencing/model_session_lease.h"
#include "inferencing/session/session.h"
#include "inferencing/session/session_runtime.h"
#include "items/message_item.h"
#include "logger.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fl {

class GenAIModelInstance;
class OnnxChatGenerator;

/// Executable body of a chat session: conversation history, cached generator and tool context.
///
/// Designed for multi-turn conversations where message history accumulates and is sent with each generation
/// request (for use with the OpenAI Responses API pattern).
///
/// Generator caching: after the first non-JSON request, the ORT GenAI generator is cached. Subsequent turns
/// append only new messages to the cached generator, reusing the KV cache. OpenAI chat completions JSON
/// requests (TextItem with text_type == OPENAI_JSON) always create a fresh generator and never use the cache.
///
/// Commit discipline: history is committed only after OperationContext::TrySeal() succeeds — i.e. after the
/// generator guard has ended, final engine reads have happened and the streaming callback has quiesced. A
/// failed seal leaves logical history unchanged but exposes generated-so-far output with FINISH_NONE to the
/// legacy ProcessRequest path; explicit operations clear that response centrally.
class ChatRuntime : public SessionRuntime {
 public:
  /// Exact state before a turn was appended to an already-existing cached generator. Absence means the turn
  /// started from a fresh generator and therefore has no valid rewind target.
  struct RewindBoundary {
    uint64_t generator_generation;
    int token_count;
  };

  /// Tracks the token-level and history-level boundaries of a single conversation turn.
  /// Used for generator rewind and history rollback on error or undo.
  struct TurnRecord {
    size_t history_start;                        // index in history_ where this turn's input messages begin
    size_t input_count;                          // number of input messages (user + tool results) in this turn
    std::optional<RewindBoundary> rewind_boundary;
    // The assistant reply is at history_[history_start + input_count]
  };

  ChatRuntime(const fl::Model& catalog_model, ModelSessionLease lease, ILogger& logger, ITelemetry& telemetry);
  ~ChatRuntime() noexcept override;

  SessionType Type() const override;

  /// Get the full conversation history.
  const std::vector<MessageItem>& GetHistory() const;

  /// Get the number of messages in the history.
  size_t MessageCount() const;

  /// Get the number of completed turns.
  size_t TurnCount() const override;

  /// Undo the last `count` completed turns: rewinds the cached generator and removes each turn's input
  /// messages and assistant reply from history. If all turns are undone, the cached generator is destroyed.
  ///
  /// Vision turns: image input is only allowed on the first turn of a session. UndoTurns rolls back history
  /// but does not undo this constraint — once a session has started, no later turn may include images. Start
  /// a new chat session to send images.
  ///
  /// @param count  Number of turns to undo. Must be <= TurnCount().
  void UndoTurns(size_t count) override;

 private:
  // populate session_options_
  void SetSessionOptionsImpl(const KeyValuePairs& options) override;

  /// Process a request: extracts MESSAGE items and parameters from the generic request,
  /// generates a response, and on a successful seal commits messages to conversation history.
  void ProcessRequestImpl(const OperationContext& operation, const Request& request, Response& response) override;

  /// Discard generator state after an exact engine cancellation delivery or a fault, once the watchdog can no
  /// longer reach it. A stop after generator withdrawal is handled by the exact rewind-boundary path instead.
  void OnRequestFinished(const OperationContext& operation, const Request& request) noexcept override;

  /// Drop the generator and the tool state baked into it. Generator destruction and ToolCallContext moves are
  /// noexcept, so this is safe on both sides of the completion seal.
  void ResetGeneratorCache() noexcept;

  /// Build tool calling context from request parameters and session tool definitions.
  ToolCallContext BuildToolCallContext(const Request& request) const;

  /// Update per-turn fields (tool_choice, guidance) on an existing tool context.
  /// Called on the cached-generator path so each turn gets fresh per-request settings
  /// while keeping session-level tool definitions and marker tokens stable.
  void UpdateToolContextForTurn(const Request& request, ToolCallContext& tool_ctx) const;

  /// Process generated output: parse tool calls (or reuse pre-parsed ones), set finish reason, usage, and response
  /// items. When `pre_parsed_calls` is non-empty, it is used as-is and no re-parse of `text` is performed — this is
  /// how the streaming path keeps `call_id`s stable: the calls parsed during streaming are also the calls returned
  /// in the final response.
  void ProcessGeneratedOutput(std::string text, const ToolCallContext& tool_ctx,
                              const SearchOptions& effective_options, bool canceled,
                              Response& response, int prompt_tokens, int total_tokens,
                              std::vector<ParsedToolCall> pre_parsed_calls = {});

  /// Process a request whose first item is a TextItem tagged OPENAI_JSON containing an OpenAI chat completions
  /// request. Parses the JSON, converts to internal items, runs generation, and produces an OPENAI_JSON-tagged
  /// TextItem response with the OpenAI ChatCompletionResponse.
  /// Does not use or update history_ or the cached generator.
  void ProcessChatCompletionsJson(const OperationContext& operation, const std::string& request_json,
                                  const Request& original_request, Response& response);

  ILogger& logger_;
  std::vector<MessageItem> history_;
  std::vector<TurnRecord> turns_;
  SearchOptions session_options_;

  // Cached generator for continuous decoding (non-JSON path only).
  // Null until first non-JSON ProcessRequestImpl call.
  std::unique_ptr<OnnxChatGenerator> cached_generator_;

  // Monotonic identity for cached_generator_, bumped every time a *new* generator is built. A rewind boundary
  // stores the generation it belongs to, so rollback can reject a token count from any rebuilt generator.
  uint64_t generator_generation_ = 0;

  // Tool context used when creating the cached generator.
  // Reused for subsequent turns to maintain tool definition consistency.
  ToolCallContext cached_tool_ctx_;
};

/// A chat session that maintains conversation history across turns.
///
/// Stateless facade over a shared ChatRuntime — see Session. Movable (it moves a shared_ptr), destroyed
/// without waiting, and it owns no model reference of its own: the runtime holds the ModelSessionLease.
class ChatSession : public Session {
 public:
  /// Compatibility construction against an already-pinned model. The caller must exclude a concurrent
  /// unload until construction returns; production paths should pass a manager-acquired ModelSessionLease.
  ChatSession(const fl::Model& catalog_model, GenAIModelInstance& model, ILogger& logger, ITelemetry& telemetry)
      : ChatSession(catalog_model, ModelSessionLease::Adopt(model), logger, telemetry) {}

  /// Construction from a lease acquired atomically against unload (Session::Create).
  ChatSession(const fl::Model& catalog_model, ModelSessionLease lease, ILogger& logger, ITelemetry& telemetry)
      : Session(MakeSessionRuntime<ChatRuntime>(catalog_model, std::move(lease), logger, telemetry)) {}

  ChatSession(ChatSession&&) noexcept = default;
  ChatSession& operator=(ChatSession&&) = delete;

  /// Get the full conversation history.
  const std::vector<MessageItem>& GetHistory() const { return Typed().GetHistory(); }

  /// Get the number of messages in the history.
  size_t MessageCount() const { return Typed().MessageCount(); }

 private:
  ChatRuntime& Typed() const { return static_cast<ChatRuntime&>(RuntimeRef()); }
};

}  // namespace fl
