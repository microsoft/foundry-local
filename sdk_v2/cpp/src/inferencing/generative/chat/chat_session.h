// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/chat/chat_transcript.h"
#include "inferencing/generative/chat/reasoning_stream_splitter.h"
#include "inferencing/generative/chat/search_options.h"
#include "inferencing/generative/chat/stop_strings.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/generative/toolcalling/tool_call_utils.h"
#include "inferencing/session/session.h"
#include "items/message_item.h"
#include "logger.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace fl {

class GenAIModelInstance;
class ChatGenerator;

namespace chat_session_internal {

template <typename SegmentProcessor>
bool PushDecodedFragment(const std::string& fragment,
                         std::optional<int32_t> token_id,
                         StopStringFilter* stop_filter,
                         ReasoningStreamSplitter& splitter,
                         SegmentProcessor&& process_segments) {
  if (stop_filter == nullptr) {
    if (token_id.has_value()) {
      process_segments(splitter.Push(*token_id, fragment));
    } else if (!fragment.empty()) {
      process_segments(splitter.Push(fragment));
    }

    return false;
  }

  if (stop_filter->matched()) {
    return true;
  }

  if (fragment.empty()) {
    if (token_id.has_value()) {
      process_segments(splitter.Push(*token_id, fragment));
    }

    return false;
  }

  auto filtered = stop_filter->PushWithTokenAlignment(fragment);
  if (!filtered.text.empty()) {
    if (filtered.token_aligned && token_id.has_value()) {
      process_segments(splitter.Push(*token_id, std::move(filtered.text)));
    } else {
      process_segments(splitter.Push(filtered.text));
    }
  }

  return stop_filter->matched();
}

template <typename SegmentProcessor>
void FlushDecodedStream(StopStringFilter* stop_filter,
                        ReasoningStreamSplitter& splitter,
                        SegmentProcessor&& process_segments) {
  if (stop_filter != nullptr && !stop_filter->matched()) {
    auto tail = stop_filter->Flush();
    if (!tail.empty()) {
      process_segments(splitter.Push(tail));
    }
  }

  process_segments(splitter.Flush());
}

flFinishReason ResolveGeneratedFinishReason(bool canceled,
                                            bool has_tool_calls,
                                            bool stop_sequence_matched,
                                            bool host_output_limit_reached,
                                            std::optional<flFinishReason> backend_finish_reason,
                                            int completion_tokens,
                                            std::optional<int> max_output_tokens);
bool DidHostOutputLimitTruncate(int output_tokens, int max_output_tokens, bool backend_finished);
bool ShouldEnforceHostOutputLimit(ChatBackendKind backend_kind, bool media_turn);
bool ShouldRebuildRetainedGeneratorBeforeAppend(ChatBackendKind backend_kind,
                                                bool guidance_requirement_changed,
                                                bool guidance_payload_changed,
                                                bool retained_generation_settings_changed);
bool ShouldInvalidateRetainedGenerationStateAfterSuccessfulTurn(ChatBackendKind backend_kind,
                                                                bool grammar_was_active,
                                                                bool reasoning_was_active,
                                                                bool stop_sequence_matched,
                                                                bool host_output_limit_reached);
bool ShouldInvalidateRetainedGeneratorForUndo(bool undo_all, bool has_pre_turn_boundary, bool can_rewind);

}  // namespace chat_session_internal

using GeneratedOutputEvent = std::variant<ReasoningStreamSplitter::Segment, ParsedToolCall>;

/// A chat session that maintains an authoritative conversation transcript across turns.
/// Designed for multi-turn conversations where visible text, reasoning, and tool calls accumulate in event order
/// and are sent with each generation request (for use with the OpenAI Responses API pattern).
///
/// Retained inference state: compatible turns reuse backend state. Engine backends render the complete authoritative
/// transcript and reuse resident tokens only when they are an exact prefix; classic Generator backends rebuild for
/// transcript shapes that cannot be appended independently.
/// OpenAI chat completions JSON requests (TextItem with text_type == OPENAI_JSON) always create a fresh
/// generator and never use the cache.
class ChatSession : public Session {
 public:
  ChatSession(const fl::Model& catalog_model, GenAIModelInstance& model, ILogger& logger, ITelemetry& telemetry,
              ChatTranscript::CommitFaultInjector transcript_fault_injector = {});
  ~ChatSession();

  // Movable: transfers session refcount ownership to the moved-to instance.
  ChatSession(ChatSession&& other) noexcept;
  ChatSession& operator=(ChatSession&&) = delete;

  SessionType Type() const override;

  /// Get the authoritative conversation transcript. Not safe to inspect while a request is in flight.
  const ChatTranscript& Transcript() const;

  /// Get the number of messages in the transcript. Not safe to inspect while a request is in flight.
  size_t MessageCount() const;

  /// Get the number of completed turns. Not safe to inspect while a request is in flight.
  size_t TurnCount() const override;

  /// Undo the last `count` completed turns: rewinds the cached generator and removes
  /// each turn's input messages and assistant reply from the transcript.
  /// If all turns are undone, the cached generator is destroyed.
  ///
  /// Vision turns: image input is only allowed while the conversation has no
  /// history. UndoTurns rolls back messages, so undoing every turn does make
  /// the session accept media again — but the media bytes of an undone turn
  /// are gone either way, because they never entered the transcript.
  ///
  /// Blocks until any in-flight request on this session completes.
  ///
  /// @param count  Number of turns to undo. Must be <= TurnCount().
  void UndoTurns(size_t count) override;

 private:
  // populate session_options_
  void SetSessionOptionsImpl(const KeyValuePairs& options) override;

  /// Process a request: extracts items and parameters from the generic request, generates a response, and on
  /// success commits the turn to the transcript.
  void ProcessRequestImpl(const Request& request, Response& response) override;

  /// Build tool calling context from request parameters and session tool definitions.
  ToolCallContext BuildToolCallContext(const Request& request) const;

  /// Build final response items from the typed segments and tool calls produced during generation.
  void ProcessGeneratedOutput(std::vector<GeneratedOutputEvent> events,
                              const SearchOptions& effective_options,
                              bool canceled,
                              bool stop_sequence_matched,
                              bool host_output_limit_reached,
                              Response& response,
                              int prompt_tokens,
                              int total_tokens,
                              int reasoning_tokens,
                              std::optional<flFinishReason> backend_finish_reason);

  /// Process a request whose first item is a TextItem tagged OPENAI_JSON containing an OpenAI chat completions
  /// request. Parses the JSON, converts to internal items, runs generation, and produces an OPENAI_JSON-tagged
  /// TextItem response with the OpenAI ChatCompletionResponse.
  /// Does not use or update the transcript or the cached generator.
  void ProcessChatCompletionsJson(const std::string& request_json, const Request& original_request,
                                  Response& response);

  /// Drop the cached generator and its tool context. Called whenever the generator's KV cache can no longer be
  /// trusted to match the committed transcript — the next turn then rebuilds from full committed history.
  /// noexcept because it also runs from a scope guard during exception unwinding.
  void InvalidateCachedGenerator() noexcept;

  GenAIModelInstance& Model() { return model_; }
  const GenAIModelInstance& Model() const { return model_; }

  ILogger& logger_;
  GenAIModelInstance& model_;
  // Tracks who is responsible for calling model_.ReleaseSession(). Set to false on the
  // moved-from instance so the refcount transfers cleanly across moves.
  bool owns_session_ = true;
  ChatTranscript transcript_;
  SearchOptions session_options_;

  // Cached generator for continuous decoding (non-JSON path only).
  // Null until first non-JSON ProcessRequestImpl call.
  std::unique_ptr<ChatGenerator> cached_generator_;

  // Tool context used when creating the cached generator.
  // Reused for subsequent turns to maintain tool definition consistency.
  ToolCallContext cached_tool_ctx_;

  // Settings baked into classic Generator state. Dynamic Engine applies supported settings per turn.
  SearchOptions cached_search_options_;

  // The system prefix baked into cached_generator_'s prompt (the kSystemPromptOption value of the turn that built
  // it). Deliberately not part of the transcript: it is request state, so it can never accumulate a copy per turn
  // and is never replayed from a stored conversation. A turn that asks for a different prefix rebuilds; a turn that
  // asks for the same one keeps the KV cache.
  std::string system_prompt_;
};

}  // namespace fl
