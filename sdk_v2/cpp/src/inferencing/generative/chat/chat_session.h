// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/search_options.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/generative/toolcalling/tool_call_utils.h"
#include "inferencing/session/session.h"
#include "items/message_item.h"
#include "logger.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fl {

class GenAIModelInstance;
class EngineChatGenerator;
class OnnxChatGenerator;

/// A chat session that maintains conversation history across turns.
/// Designed for multi-turn conversations where message history accumulates
/// and is sent with each generation request (for use with the OpenAI
/// Responses API pattern).
///
/// Generator caching: eligible dynamic-batching text models retain an Engine request between non-JSON turns.
/// Other text models retain the existing OgaGenerator cache.
/// OpenAI chat completions JSON requests (TextItem with text_type == OPENAI_JSON) always create a fresh
/// generator and never use the cache.
class ChatSession : public Session {
 public:
  /// Tracks the history boundary of a committed conversation turn for UndoTurns.
  struct TurnRecord {
    size_t history_start;  // index in history_ where this turn's input messages begin
    size_t input_count;    // number of input messages (user + tool results) in this turn
    // The assistant reply is at history_[history_start + input_count]
  };

  ChatSession(const fl::Model& catalog_model, GenAIModelInstance& model, ILogger& logger, ITelemetry& telemetry);
  ~ChatSession();

  // Movable: transfers session refcount ownership to the moved-to instance.
  ChatSession(ChatSession&& other) noexcept;
  ChatSession& operator=(ChatSession&&) = delete;

  SessionType Type() const override;

  /// Get the full conversation history.
  const std::vector<MessageItem>& GetHistory() const;

  /// Get the number of messages in the history.
  size_t MessageCount() const;

  /// Get the number of completed turns.
  size_t TurnCount() const override;

  /// Undo the last `count` completed turns and remove each turn's input messages and assistant reply from history.
  /// Cached generation state is closed; the next turn safely replays the retained history.
  ///
  /// Vision turns: image input is only allowed on the first turn of a
  /// session. UndoTurns rolls back history but does not undo this
  /// constraint — once a session has started, no later turn may include
  /// images. Start a new ChatSession to send images.
  ///
  /// @param count  Number of turns to undo. Must be <= TurnCount().
  void UndoTurns(size_t count) override;

 private:
  // populate session_options_
  void SetSessionOptionsImpl(const KeyValuePairs& options) override;

  /// Process a request: extracts MESSAGE items and parameters from the generic request,
  /// generates a response, and on success commits messages to conversation history.
  void ProcessRequestImpl(const Request& request, Response& response) override;

  /// Build tool calling context from request parameters and session tool definitions.
  ToolCallContext BuildToolCallContext(const Request& request) const;

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
  void ProcessChatCompletionsJson(const std::string& request_json, const Request& original_request,
                                  Response& response);

  /// Commit input messages and assistant reply to history after a successful turn.
  void CommitTurn(std::vector<MessageItem>&& new_messages, const Response& response);

  /// Prepare an exact-prefix Engine continuation or replace it with a fresh resident request replayed from history.
  EngineChatGenerator& PrepareResidentGenerator(const std::vector<MessageItem>& all_messages,
                                                const SearchOptions& options,
                                                const ToolCallContext& tool_ctx,
                                                const Request& request);

  /// Release every cached generation state. Destructors perform no-throw backend cleanup.
  void ResetGeneratorCache() noexcept;

  GenAIModelInstance& Model() { return model_; }
  const GenAIModelInstance& Model() const { return model_; }

  ILogger& logger_;
  GenAIModelInstance& model_;
  // Tracks who is responsible for calling model_.ReleaseSession(). Set to false on the
  // moved-from instance so the refcount transfers cleanly across moves.
  bool owns_session_ = true;
  std::vector<MessageItem> history_;
  std::vector<TurnRecord> turns_;
  SearchOptions session_options_;

  // Fallback OgaGenerator cache for non-engine, non-JSON turns.
  std::unique_ptr<OnnxChatGenerator> cached_generator_;

  // Resident Engine request for eligible text-only dynamic-batching models.
  std::unique_ptr<EngineChatGenerator> resident_generator_;

  // Configuration bound to the currently cached generator/request.
  ToolCallContext cached_tool_ctx_;
  std::optional<SearchOptions> cached_options_;
};

}  // namespace fl
