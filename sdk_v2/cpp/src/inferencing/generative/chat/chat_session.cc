// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/chat/chat_session.h"

#include "contracts/chat_completions.h"
#include "contracts/chat_completions_converter.h"
#include "inferencing/generative/chat/onnx_chat_generator.h"
#include "inferencing/generative/chat/reasoning_stream_splitter.h"
#include "inferencing/generative/genai_model_instance.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/generative/toolcalling/tool_call_stream_accumulator.h"
#include "inferencing/generative/toolcalling/tool_call_utils.h"
#include "items/image_item.h"
#include "items/text_item.h"
#include "items/tool_result_item.h"
#include "items/tool_call_item.h"
#include "model.h"
#include "utils.h"

#include <chrono>
#include <fmt/format.h>
#include <utility>

namespace fl {

namespace {

// Translate a parsed tool_choice into the text_output / tool_output flags on the tool-call context.
// Defaults (nullopt) match "auto": the model is free to emit text or tool calls.
void ApplyToolChoiceToContext(std::optional<flToolChoice> tool_choice, ToolCallContext& tool_ctx) {
  switch (tool_choice.value_or(FOUNDRY_LOCAL_TOOL_CHOICE_AUTO)) {
    case FOUNDRY_LOCAL_TOOL_CHOICE_NONE:
      tool_ctx.text_output = true;
      tool_ctx.tool_output = false;
      break;
    case FOUNDRY_LOCAL_TOOL_CHOICE_REQUIRED:
      tool_ctx.text_output = false;
      tool_ctx.tool_output = true;
      break;
    case FOUNDRY_LOCAL_TOOL_CHOICE_AUTO:
    default:
      tool_ctx.text_output = true;
      tool_ctx.tool_output = true;
      break;
  }
}

}  // namespace

ChatSession::ChatSession(const fl::Model& catalog_model, GenAIModelInstance& model, ILogger& logger, ITelemetry& telemetry)
    : Session(catalog_model, logger, telemetry), logger_(logger), model_(model) {
  logger_.Log(LogLevel::Debug, fmt::format("Creating ChatSession for model: {}", model.ModelId()));
  // Last so a throw above does not leak a refcount; nothing below can throw.
  model_.AcquireSession();
}

ChatSession::~ChatSession() {
  if (owns_session_) {
    model_.ReleaseSession();
  }
}

ChatSession::ChatSession(ChatSession&& other) noexcept
    : Session(std::move(other)),
      logger_(other.logger_),
      model_(other.model_),
      owns_session_(other.owns_session_),
      history_(std::move(other.history_)),
      turns_(std::move(other.turns_)),
      session_options_(std::move(other.session_options_)),
      cached_generator_(std::move(other.cached_generator_)) {
  other.owns_session_ = false;
}

SessionType ChatSession::Type() const {
  return SessionType::kChat;
}

void ChatSession::Cancel() {
  Session::Cancel();

  std::lock_guard<std::mutex> lock(active_generator_mutex_);
  if (active_generator_ != nullptr) {
    active_generator_->Cancel();
  }
}

void ChatSession::SetActiveGenerator(OnnxChatGenerator* generator) {
  std::lock_guard<std::mutex> lock(active_generator_mutex_);
  active_generator_ = generator;
}

void ChatSession::ClearActiveGenerator(OnnxChatGenerator* generator) {
  std::lock_guard<std::mutex> lock(active_generator_mutex_);
  if (active_generator_ == generator) {
    active_generator_ = nullptr;
  }
}

void ChatSession::SetSessionOptionsImpl(const KeyValuePairs& options) {
  session_options_ = SearchOptions::FromParameters(options);
}

void ChatSession::UpdateToolContextForTurn(const Request& request, ToolCallContext& tool_ctx) const {
  auto get_param = [&](const char* key) -> std::string {
    auto it = request.options.find(key);
    if (it != request.options.end()) {
      return it->second;
    }
    return {};
  };

  // Re-derive tool_choice → text_output / tool_output for this turn.
  // ParseToolChoice rejects unknown values with FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT.
  auto tool_choice = SearchOptions::ParseToolChoice(request.options);

  if (tool_ctx.HasTools()) {
    ApplyToolChoiceToContext(tool_choice, tool_ctx);
  }

  // Re-derive per-request guidance
  tool_ctx.guidance_type = get_param("guidance_type");
  tool_ctx.guidance_data = get_param("guidance_data");
}

ToolCallContext ChatSession::BuildToolCallContext(const Request& request) const {
  ToolCallContext tool_ctx;

  auto get_param = [&](const char* key) -> std::string {
    auto it = request.options.find(key);
    if (it != request.options.end()) {
      return it->second;
    }
    return {};
  };

  tool_ctx.tool_call_start = get_param(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_START_STR);
  tool_ctx.tool_call_end = get_param(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_END_STR);

  // Fall back to model info properties if not specified in the request
  const auto& info = CatalogModel().Info();

  // Check if the model supports tool calling
  const auto* tool_calling_val = info.GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT);
  if (tool_calling_val && *tool_calling_val == 1) {
    tool_ctx.supports_tool_calling = true;
  }

  if (tool_ctx.tool_call_start.empty()) {
    const auto* val = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_START_STR);
    if (val) {
      tool_ctx.tool_call_start = *val;
    }
  }

  if (tool_ctx.tool_call_end.empty()) {
    const auto* val = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_END_STR);
    if (val) {
      tool_ctx.tool_call_end = *val;
    }
  }

  // Check if the model supports chain-of-thought reasoning
  const auto* reasoning_val = info.GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT);
  if (reasoning_val && *reasoning_val == 1) {
    tool_ctx.supports_reasoning = true;
  }

  // Read reasoning marker tokens — same pattern as tool_call tokens
  tool_ctx.reasoning_start = get_param(FOUNDRY_LOCAL_MODEL_PROP_REASONING_START_STR);
  tool_ctx.reasoning_end = get_param(FOUNDRY_LOCAL_MODEL_PROP_REASONING_END_STR);

  if (tool_ctx.reasoning_start.empty()) {
    const auto* val = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_REASONING_START_STR);
    if (val) {
      tool_ctx.reasoning_start = *val;
    }
  }

  if (tool_ctx.reasoning_end.empty()) {
    const auto* val = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_REASONING_END_STR);
    if (val) {
      tool_ctx.reasoning_end = *val;
    }
  }

  // Accumulate tool definitions from the session.
  // Tool definitions may come from two sources:
  // 1. Individual AddToolDefinition calls (name + description + parameters schema)
  // 2. ChatCompletions converter (pre-serialized full OpenAI tools JSON array, no name)
  // We need to produce a JSON array in OpenAI tools format for the chat template.
  nlohmann::json tools_array = nlohmann::json::array();
  bool has_preserialized = false;

  for (const auto& td : ToolDefinitions()) {
    if (!td.name.empty()) {
      // Individual tool: wrap in OpenAI format
      nlohmann::json tool;
      tool["type"] = "function";
      tool["function"]["name"] = td.name;
      tool["function"]["description"] = td.description;

      if (!td.json_schema.empty()) {
        tool["function"]["parameters"] = nlohmann::json::parse(td.json_schema);
      }

      tools_array.push_back(std::move(tool));
    } else if (!td.json_schema.empty()) {
      // Pre-serialized from ChatCompletions path — already a complete tools array
      has_preserialized = true;
      tool_ctx.tools_json += td.json_schema;
    }
  }

  if (!tools_array.empty()) {
    tool_ctx.tools_json = tools_array.dump();
  } else if (!has_preserialized) {
    tool_ctx.tools_json.clear();
  }

  // Determine text_output / tool_output from tool_choice parameter.
  // ParseToolChoice rejects unknown values with FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT.
  auto tool_choice = SearchOptions::ParseToolChoice(request.options);

  if (tool_ctx.HasTools()) {
    ApplyToolChoiceToContext(tool_choice, tool_ctx);
  }

  // Read user-specified guidance from request parameters
  tool_ctx.guidance_type = get_param("guidance_type");
  tool_ctx.guidance_data = get_param("guidance_data");

  return tool_ctx;
}

// A segment of generated assistant text, tagged with whether it is ordinary visible text or reasoning content.
struct TextSegment {
  std::string text;
  flTextItemType type;
};

// Split assistant output around <think>...</think> (or equivalent reasoning markers) into typed segments.
//
// - Text outside the markers is emitted as DEFAULT (visible) segments.
// - Text inside the markers is emitted as REASONING segments. The markers themselves are stripped.
// - Truncated reasoning (no closing marker) is treated as a REASONING segment running to end of input.
// - Empty segments are skipped.
// - When start_marker is empty, the entire input is returned as a single DEFAULT segment.
//
// Leading whitespace/newlines on visible segments that immediately follow a closed reasoning block are trimmed —
// matches the prior strip behavior, which dropped a trailing newline after </think> plus any leading whitespace.
static std::vector<TextSegment> SplitReasoningContent(const std::string& text,
                                                      const std::string& start_marker,
                                                      const std::string& end_marker) {
  std::vector<TextSegment> segments;

  if (start_marker.empty() || text.empty()) {
    if (!text.empty()) {
      segments.push_back({text, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT});
    }
    return segments;
  }

  auto push_default = [&](std::string s) {
    // Trim leading whitespace from visible segments (prior strip logic dropped these).
    size_t first = s.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) {
      return;
    }
    s.erase(0, first);
    if (!s.empty()) {
      segments.push_back({std::move(s), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT});
    }
  };

  auto push_reasoning = [&](std::string s) {
    if (!s.empty()) {
      segments.push_back({std::move(s), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING});
    }
  };

  size_t pos = 0;
  while (pos < text.size()) {
    size_t start_pos = text.find(start_marker, pos);

    if (start_pos == std::string::npos) {
      push_default(text.substr(pos));
      break;
    }

    // Visible text before the reasoning block.
    push_default(text.substr(pos, start_pos - pos));

    size_t reasoning_begin = start_pos + start_marker.size();
    size_t end_pos = text.find(end_marker, reasoning_begin);

    if (end_pos == std::string::npos) {
      // Truncated — reasoning runs to end of string. Drop nothing; expose what we have.
      push_reasoning(text.substr(reasoning_begin));
      break;
    }

    push_reasoning(text.substr(reasoning_begin, end_pos - reasoning_begin));
    pos = end_pos + end_marker.size();

    // Drop a single trailing newline after </think> (matches prior strip behavior).
    if (pos < text.size() && text[pos] == '\n') {
      ++pos;
    }
  }

  return segments;
}

void ChatSession::ProcessGeneratedOutput(std::string text, const ToolCallContext& tool_ctx,
                                         const SearchOptions& effective_options, bool canceled,
                                         Response& response, int prompt_tokens, int total_tokens,
                                         std::vector<ParsedToolCall> pre_parsed_calls) {
  int completion_tokens = total_tokens - prompt_tokens;

  // Check if the generated text contains tool calls. If the caller has already parsed them (streaming path), reuse
  // those so call_ids stay stable across stream deltas and the final response — OpenAI Chat Completions semantics.
  bool has_tool_calls = false;
  std::vector<ParsedToolCall> parsed_calls;

  if (!pre_parsed_calls.empty()) {
    parsed_calls = std::move(pre_parsed_calls);
    has_tool_calls = true;
  } else if (tool_ctx.HasTools() && tool_ctx.tool_output && tool_ctx.HasToolCallTokens()) {
    parsed_calls = ParseToolCalls(text, tool_ctx.tool_call_start, tool_ctx.tool_call_end);
    has_tool_calls = !parsed_calls.empty();
  }

  if (has_tool_calls) {
    // Add structured tool call items to the response
    auto tool_items = ToolCallsToItems(parsed_calls);
    for (auto& ti : tool_items) {
      response.items.push_back(std::move(ti));
    }
  }

  // Split assistant output around reasoning markers so reasoning content can be returned to the caller as a typed
  // TextItem alongside the visible response text. RenderContent in chat_template.cc skips REASONING parts when
  // re-applying the template to history, so storing reasoning here doesn't contaminate subsequent prompts.
  std::vector<TextSegment> segments;

  if (tool_ctx.supports_reasoning) {
    std::string start = tool_ctx.reasoning_start.empty() ? "<think>" : tool_ctx.reasoning_start;
    std::string end = tool_ctx.reasoning_end.empty() ? "</think>" : tool_ctx.reasoning_end;
    segments = SplitReasoningContent(text, start, end);
  } else if (!text.empty()) {
    segments.push_back({std::move(text), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT});
  }

  // Build the assistant message from the segments. Tool-call-only outputs may produce zero segments — emit no
  // message in that case, since MessageItem requires non-empty content.
  if (!segments.empty()) {
    std::unique_ptr<MessageItem> output_item;

    if (segments.size() == 1 && segments.front().type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT) {
      // Common case: pure visible text → single-text MessageItem.
      output_item = std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(segments.front().text));
    } else {
      // Mixed / reasoning-only → multi-part MessageItem of typed TextItems.
      std::vector<std::unique_ptr<Item>> parts;
      parts.reserve(segments.size());
      for (auto& seg : segments) {
        parts.push_back(std::make_unique<TextItem>(std::move(seg.text), seg.type));
      }
      output_item = std::make_unique<MessageItem>(FOUNDRY_LOCAL_ROLE_ASSISTANT, std::move(parts));
    }

    response.items.push_back(std::move(output_item));
  }

  if (canceled) {
    response.finish_reason = FOUNDRY_LOCAL_FINISH_NONE;
  } else if (has_tool_calls) {
    response.finish_reason = FOUNDRY_LOCAL_FINISH_TOOL_CALLS;
  } else {
    int max_output = effective_options.max_output_tokens.value_or(0);

    if (max_output > 0 && completion_tokens >= max_output) {
      response.finish_reason = FOUNDRY_LOCAL_FINISH_LENGTH;
    } else {
      response.finish_reason = FOUNDRY_LOCAL_FINISH_STOP;
    }
  }

  response.usage.prompt_tokens = prompt_tokens;
  response.usage.completion_tokens = completion_tokens;
  response.usage.total_tokens = total_tokens;

  logger_.Log(LogLevel::Verbose,
              fmt::format("Completion stats: Total Tokens: {}, Prompt Tokens: {}, Completion Tokens: {}",
                          total_tokens, prompt_tokens, completion_tokens));
}

void ChatSession::ProcessRequestImpl(const Request& request, Response& response) {
  // OpenAI chat completions JSON pass-through: a TEXT item tagged OPENAI_JSON. Routes to a separate handler that
  // never uses the cached generator or history (the JSON payload is self-contained).
  for (const auto* item : request.items) {
    if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
      const auto& text_item = static_cast<const fl::TextItem&>(*item);

      if (text_item.text_type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON) {
        ProcessChatCompletionsJson(text_item.text, request, response);
        return;
      }
    }
  }

  // Collect new input messages locally — NOT in history_ yet.
  // History is only committed on successful generation (delayed commit).
  std::vector<MessageItem> new_messages;
  for (const auto* item : request.items) {
    if (item->type == FOUNDRY_LOCAL_ITEM_MESSAGE) {
      auto& message_item = static_cast<const fl::MessageItem&>(*item);
      if (!message_item.content.empty()) {
        new_messages.push_back(message_item);
      }
    } else if (item->type == FOUNDRY_LOCAL_ITEM_TOOL_RESULT) {
      // Convert tool result to a message with role="tool" for the chat template
      auto& tool_result = static_cast<const fl::ToolResultItem&>(*item);
      if (!tool_result.result.empty()) {
        new_messages.emplace_back(FOUNDRY_LOCAL_ROLE_TOOL, tool_result.result);
      }
    }
  }

  if (new_messages.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "At least one MESSAGE item with non-empty content is required in the request");
  }

  // Vision input detection.
  //
  // Image input is only allowed on the first turn of a session. After that:
  //   - AppendMessages (continuous decoding) has no image-aware path; image
  //     parts in subsequent turns would be silently dropped.
  //   - Conversation history can't replay image bytes through the chat
  //     template, so we can't even rebuild from scratch with prior images.
  // The simplest correct behaviour is to require a fresh session for every
  // vision request. Text follow-ups within the same session are fine — the
  // model has already produced a textual description that lives in history.
  std::vector<const ImageItem*> images;
  for (const auto& msg : new_messages) {
    for (const auto& part : msg.content) {
      if (part.view && part.view->type == FOUNDRY_LOCAL_ITEM_IMAGE) {
        images.push_back(static_cast<const ImageItem*>(part.view));
      }
    }
  }

  const bool vision_turn = !images.empty();

  if (vision_turn && !history_.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "image input is only allowed on the first turn of a session; "
             "create a new ChatSession to send images");
  }

  // Merge session-level and per-request options once for this turn.
  auto effective_kvp = MergedOptions(request.options);
  SearchOptions effective_options = SearchOptions::FromParameters(effective_kvp);

  int prompt_tokens = 0;
  int pre_turn_token_count = 0;

  if (cached_generator_) {
    // Check if guidance requirements changed since the generator was created. Guidance (LARK grammar) is baked into
    // the OGA generator at creation time and cannot be changed. If tool_choice went from "required" to "auto" (or
    // vice versa), we must recreate the generator from full history.
    auto turn_tool_ctx = cached_tool_ctx_;
    UpdateToolContextForTurn(request, turn_tool_ctx);

    bool prev_needs_guidance = cached_tool_ctx_.tool_output && !cached_tool_ctx_.text_output;
    bool curr_needs_guidance = turn_tool_ctx.tool_output && !turn_tool_ctx.text_output;

    if (prev_needs_guidance != curr_needs_guidance) {
      // Guidance requirements changed — invalidate. The branch below will rebuild from full history.
      cached_generator_.reset();
      cached_tool_ctx_ = {};
    } else {
      // Continuous decoding: append only the new messages to the existing generator.
      pre_turn_token_count = cached_generator_->TokenCount();
      prompt_tokens = cached_generator_->AppendMessages(new_messages, Model(), cached_tool_ctx_.tools_json);

      // Refresh per-turn fields (tool_choice, guidance) while keeping session-level definitions stable.
      UpdateToolContextForTurn(request, cached_tool_ctx_);
    }
  }

  if (!cached_generator_) {
    // First request (or cache invalidated): create the generator from scratch.
    // Combine existing history with new messages for the full context.
    auto tool_ctx = BuildToolCallContext(request);

    std::vector<MessageItem> all_messages;
    all_messages.reserve(history_.size() + new_messages.size());
    all_messages.insert(all_messages.end(), history_.begin(), history_.end());
    all_messages.insert(all_messages.end(), new_messages.begin(), new_messages.end());

    std::unique_ptr<OnnxChatGenerator> generator;
    if (vision_turn) {
      // Vision is single-shot: the generator is dropped after the turn (see
      // CommitTurn cleanup below) because AppendMessages can't extend a
      // sequence whose state includes image-derived tokens. Sizing the KV
      // cache to the model's full context window would needlessly allocate
      // gigabytes (262k tokens × 28 layers × 8 heads × 128 dims for
      // qwen3-vl-2b ≈ 120 GB). Bound it to prompt + max_output_tokens.
      generator = OnnxChatGenerator::CreateWithImages(all_messages, effective_options, Model(), images, tool_ctx,
                                                      /*use_full_context*/ false);
    } else {
      generator = OnnxChatGenerator::Create(all_messages, effective_options, Model(), tool_ctx,
                                            /*use_full_context*/ true);
    }
    prompt_tokens = generator->PromptTokenCount();

    cached_generator_ = std::move(generator);
    cached_tool_ctx_ = std::move(tool_ctx);
  }

  int max_output = effective_options.max_output_tokens.value_or(0);

  // Generate token-by-token with optional streaming.
  // Check request.canceled each iteration — a streaming callback returning
  // non-zero sets this flag asynchronously via CallbackHandler.
  std::string text;
  auto streaming_callback = CreateCallbackHandler(request);
  int output_tokens = 0;

  // Splitter: only active for reasoning models. For non-reasoning models start_marker is empty and the splitter
  // degrades to a passthrough (every token becomes one DEFAULT segment), so the streaming path stays uniform.
  ReasoningStreamSplitter splitter(
      cached_tool_ctx_.supports_reasoning ? (cached_tool_ctx_.reasoning_start.empty() ? std::string("<think>")
                                                                                      : cached_tool_ctx_.reasoning_start)
                                          : std::string(),
      cached_tool_ctx_.supports_reasoning ? (cached_tool_ctx_.reasoning_end.empty() ? std::string("</think>")
                                                                                    : cached_tool_ctx_.reasoning_end)
                                          : std::string());

  // Accumulator: separates visible text from tool-call blocks in the DEFAULT-segment stream. For models without
  // tool-call markers configured, both marker strings are empty and the accumulator degrades to passthrough.
  // REASONING segments bypass the accumulator entirely — tool-call-shaped text inside <think>...</think> is the
  // model's scratchpad and is not a real tool call.
  ToolCallStreamAccumulator tool_accumulator(cached_tool_ctx_.tool_call_start, cached_tool_ctx_.tool_call_end);

  // Tool calls parsed during streaming. Reused by ProcessGeneratedOutput so call_ids stay stable across stream
  // deltas and the final response (OpenAI Chat Completions contract). Populated even when there is no streaming
  // callback — the accumulator still parses on close — but in that case the final-response path re-parses anyway,
  // which is fine because the IDs only need to be stable when a client is observing the stream.
  std::vector<ParsedToolCall> streamed_tool_calls;

  auto emit_segments = [&](const std::vector<ReasoningStreamSplitter::Segment>& segments) {
    for (const auto& seg : segments) {
      if (seg.type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING) {
        // REASONING goes straight through — never feed it to the tool-call accumulator.
        if (streaming_callback) {
          streaming_callback->PushItem(std::make_unique<TextItem>(seg.text, seg.type));
        }
        continue;
      }

      auto out = tool_accumulator.Push(seg.text);

      if (streaming_callback && !out.visible_text.empty()) {
        streaming_callback->PushItem(
            std::make_unique<TextItem>(std::move(out.visible_text), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT));
      }

      for (auto& pc : out.ready_calls) {
        if (streaming_callback) {
          streaming_callback->PushItem(std::make_unique<ToolCallItem>(pc.id, pc.name, pc.arguments));
        }
        streamed_tool_calls.push_back(std::move(pc));
      }
    }
  };

  auto flush_accumulator = [&]() {
    auto out = tool_accumulator.Flush();

    if (streaming_callback && !out.visible_text.empty()) {
      streaming_callback->PushItem(
          std::make_unique<TextItem>(std::move(out.visible_text), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT));
    }

    for (auto& pc : out.ready_calls) {
      if (streaming_callback) {
        streaming_callback->PushItem(std::make_unique<ToolCallItem>(pc.id, pc.name, pc.arguments));
      }
      streamed_tool_calls.push_back(std::move(pc));
    }
  };

  OnnxChatGenerator* active_generator = cached_generator_.get();
  SetActiveGenerator(active_generator);
  try {
    while (!cached_generator_->IsDone() && !request.canceled && !IsCancellationRequested()) {
      cached_generator_->GenerateNextToken();
      std::string token = cached_generator_->Decode();
      ++output_tokens;

      if (!token.empty()) {
        text += token;
        emit_segments(splitter.Push(token));
      }

      // Enforce max_output_tokens — with use_full_context the OGA max_length
      // is the entire context window, so we must cap output ourselves.
      if (max_output > 0 && output_tokens >= max_output) {
        break;
      }
    }
  } catch (...) {
    ClearActiveGenerator(active_generator);
    throw;
  }
  ClearActiveGenerator(active_generator);

  const bool canceled = request.canceled || IsCancellationRequested();

  // End-of-stream: drain the reasoning splitter first so any final DEFAULT bytes feed into the tool accumulator,
  // then drain the tool accumulator.
  emit_segments(splitter.Flush());
  flush_accumulator();

  int total_tokens = cached_generator_->TokenCount();

  if (canceled) {
    // Rewind the generator to undo this turn's input. The generator remains valid
    // for the next attempt — the caller can re-send the same input.
    cached_generator_->RewindTo(pre_turn_token_count);
  }

  ProcessGeneratedOutput(std::move(text), cached_tool_ctx_, effective_options, canceled,
                         response, prompt_tokens, total_tokens, std::move(streamed_tool_calls));

  // Commit input messages + assistant reply to history only on success (not cancelled)
  if (!canceled) {
    // LARK grammar (tool-call-only mode) is a single-shot finite parse. If generation was truncated while grammar was
    // active, the parser is in an unrecoverable state. Additionally, a completed grammar signals EOS — IsDone() would
    // return true on the next turn. Invalidate after any grammar-guided generation so the next turn rebuilds.
    //
    // Reasoning models (qwen3, etc.) also need invalidation: continuous decoding leaves prior <think> tokens in the KV
    // cache and the model fails to close subsequent reasoning blocks. The chat template strips prior </think> content
    // when re-applied to history, so a rebuild restores correct behavior. This matches C#, which always applies the
    // full template per turn.
    bool grammar_was_active = cached_tool_ctx_.tool_output && !cached_tool_ctx_.text_output;
    bool reasoning_was_active = cached_tool_ctx_.supports_reasoning;

    if (grammar_was_active || reasoning_was_active) {
      cached_generator_.reset();
      cached_tool_ctx_ = {};
    }

    CommitTurn(std::move(new_messages), response, pre_turn_token_count, total_tokens);

    // After a vision turn, drop the cached generator so any text follow-up
    // rebuilds from history. AppendMessages cannot extend a vision-decoded
    // sequence; trying to do so would silently feed text into a state that
    // includes image-derived tokens.
    if (vision_turn) {
      cached_generator_.reset();
      cached_tool_ctx_ = {};
    }
  }
}

void ChatSession::ProcessChatCompletionsJson(const std::string& request_json, const Request& original_request,
                                             Response& response) {
  // Parse the OpenAI chat completions request
  auto req_json = nlohmann::json::parse(request_json);
  auto req = req_json.get<ChatCompletionRequest>();

  // Apply catalog defaults passed via request options
  chat_completions::ApplyCatalogDefaults(req, CatalogModel().Info().model_settings);

  std::string model_name = req.model;
  std::string completion_id = chat_completions::GenerateCompletionId();
  auto now = std::chrono::system_clock::now();
  int64_t created = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

  // Build the internal request from the chat completions request
  Request internal_request;

  // We don't use history_ for this request as it's for backwards compat and all messages come from the input.
  chat_completions::BuildRequestItems(req, internal_request);
  if (internal_request.items.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "At least one MESSAGE item with non-empty content is required in the request");
  }

  std::string tools_json = chat_completions::ExtractToolDefinitions(req, internal_request);
  chat_completions::MapRequestParameters(req, internal_request);
  chat_completions::MapGuidance(req, internal_request);
  chat_completions::MapStopSequences(req, internal_request);

  // Merge options from the original request (e.g. tool_call_start/end from model properties)
  for (const auto& [key, value] : original_request.options) {
    if (internal_request.options.find(key) == internal_request.options.end()) {
      internal_request.options[key] = value;
    }
  }

  // Build tool call context
  if (!tools_json.empty()) {
    // we don't expect a Session to get re-used on this path so this should always be empty
    if (ToolDefinitions().size() > 0) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
               "Tool definitions cannot be used with OpenAI JSON input; the JSON payload must be fully self-contained");
    }

    AddToolDefinition({{}, {}, std::move(tools_json)});
  }

  auto tool_ctx = BuildToolCallContext(internal_request);

  // Merge session-level and per-request options once.
  auto effective_kvp = MergedOptions(internal_request.options);
  SearchOptions options = SearchOptions::FromParameters(effective_kvp);

  // Collect MessageItems from the internal request for the generator.
  // We don't use history_ here — all messages come from the parsed JSON input.
  std::vector<MessageItem> messages;
  for (const auto* item : internal_request.items) {
    if (item->type == FOUNDRY_LOCAL_ITEM_MESSAGE) {
      messages.push_back(static_cast<const MessageItem&>(*item));
    }
  }

  // Create generator
  auto generator = OnnxChatGenerator::Create(messages, options, Model(), tool_ctx);
  int prompt_tokens = generator->PromptTokenCount();

  auto streaming_callback = CreateCallbackHandler(original_request);
  bool is_streaming = (streaming_callback != nullptr);

  // Emit initial streaming chunk
  if (is_streaming) {
    auto initial_json = chat_completions::FormatInitialStreamingChunk(completion_id, created, model_name);
    streaming_callback->PushItem(std::make_unique<TextItem>(std::move(initial_json),
                                                            FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
  }

  // Tool-call accumulator: parses tool-call blocks out of the visible stream. Empty markers degrade to passthrough.
  // Replaces a prior inline accumulator that did exact per-segment marker matching — that only worked because the
  // qwen tokenizer happens to emit `<tool_call>` as a single special token. Tokenizers that split the marker across
  // multiple tokens (or chat templates that produce marker-shaped text gradually) would silently fail. The shared
  // accumulator buffers across tokens and is verified by unit tests.
  ToolCallStreamAccumulator tool_accumulator(tool_ctx.tool_call_start, tool_ctx.tool_call_end);

  // Tool calls parsed during streaming. Reused by ProcessGeneratedOutput so call_ids stay stable across stream
  // deltas and the final ChatCompletionResponse (OpenAI Chat Completions contract).
  std::vector<ParsedToolCall> streamed_tool_calls;

  // Reasoning-aware token splitter. For non-reasoning models the splitter is a passthrough (every token becomes one
  // DEFAULT segment) so the loop body is uniform. For reasoning models, REASONING segments are suppressed from the
  // Chat Completions stream — the OpenAI Chat Completions spec has no reasoning-delta concept; reasoning is exposed
  // via the Responses API path in Stage 4. The non-streaming response already excludes reasoning text from
  // `delta.content` via the typed-MessageItem build in ProcessGeneratedOutput.
  ReasoningStreamSplitter splitter(
      tool_ctx.supports_reasoning ? (tool_ctx.reasoning_start.empty() ? std::string("<think>")
                                                                      : tool_ctx.reasoning_start)
                                  : std::string(),
      tool_ctx.supports_reasoning ? (tool_ctx.reasoning_end.empty() ? std::string("</think>")
                                                                    : tool_ctx.reasoning_end)
                                  : std::string());

  auto emit_visible_text = [&](std::string visible) {
    if (visible.empty() || !is_streaming) {
      return;
    }

    auto chunk_json = chat_completions::FormatStreamingChunk(visible, completion_id, created, model_name);
    streaming_callback->PushItem(std::make_unique<TextItem>(std::move(chunk_json),
                                                            FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
  };

  auto emit_ready_calls = [&](std::vector<ParsedToolCall>& ready) {
    if (ready.empty()) {
      return;
    }

    if (is_streaming) {
      std::vector<ChatCompletionToolCall> tc_list;
      tc_list.reserve(ready.size());
      int tc_index = 0;

      for (const auto& pc : ready) {
        ChatCompletionToolCall tc;
        tc.index = tc_index++;
        tc.id = pc.id;
        tc.type = "function";
        tc.function.name = pc.name;
        tc.function.arguments = pc.arguments;
        tc_list.push_back(std::move(tc));
      }

      auto chunk_json = chat_completions::FormatToolCallStreamingChunk(tc_list, completion_id, created, model_name);
      streaming_callback->PushItem(std::make_unique<TextItem>(std::move(chunk_json),
                                                              FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
    }

    for (auto& pc : ready) {
      streamed_tool_calls.push_back(std::move(pc));
    }
  };

  auto process_segments = [&](const std::vector<ReasoningStreamSplitter::Segment>& segments) {
    for (const auto& seg : segments) {
      // REASONING segments: intentionally dropped from the Chat Completions stream. Never feed reasoning text to
      // the tool-call accumulator — tool-call-shaped text inside <think>...</think> is scratchpad, not a real call.
      if (seg.type != FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT) {
        continue;
      }

      auto out = tool_accumulator.Push(seg.text);
      emit_visible_text(std::move(out.visible_text));
      emit_ready_calls(out.ready_calls);
    }
  };

  // Generate token-by-token
  std::string text;
  SetActiveGenerator(generator.get());
  try {
    while (!generator->IsDone() && !original_request.canceled && !IsCancellationRequested()) {
      generator->GenerateNextToken();
      std::string token = generator->Decode();

      if (!token.empty()) {
        text += token;
        process_segments(splitter.Push(token));
      }
    }
  } catch (...) {
    ClearActiveGenerator(generator.get());
    throw;
  }
  ClearActiveGenerator(generator.get());

  const bool canceled = original_request.canceled || IsCancellationRequested();

  // Drain any buffered partial-marker bytes at end-of-stream. Reasoning splitter first so any final DEFAULT bytes
  // feed into the tool accumulator; then drain the tool accumulator.
  process_segments(splitter.Flush());
  {
    auto out = tool_accumulator.Flush();
    emit_visible_text(std::move(out.visible_text));
    emit_ready_calls(out.ready_calls);
  }

  int total_tokens = generator->TokenCount();

  // Process the generated output into response items (MessageItem, ToolCallItem, etc.)
  // This also updates finish_reason, and usage on the response. Streamed-parsed tool calls are reused so call_ids
  // stay stable across stream deltas and the final ChatCompletionResponse.
  ProcessGeneratedOutput(std::move(text), tool_ctx, options, canceled,
                         response, prompt_tokens, total_tokens, std::move(streamed_tool_calls));

  // Emit final streaming chunk with finish_reason
  if (is_streaming) {
    auto final_json = chat_completions::FormatFinalStreamingChunk(response.finish_reason, completion_id, created,
                                                                  model_name);
    streaming_callback->PushItem(std::make_unique<TextItem>(std::move(final_json),
                                                            FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
  }

  // Store completion envelope metadata so callers can access without parsing JSON
  response.metadata["completion_id"] = completion_id;
  response.metadata["created"] = std::to_string(created);
  response.metadata["model"] = model_name;

  // Build the ChatCompletionResponse and replace response items with a single OPENAI_JSON-tagged TextItem.
  auto chat_response = chat_completions::BuildResponse(response, completion_id, created, model_name);
  response.items.clear();
  response.items.push_back(std::make_unique<TextItem>(nlohmann::json(chat_response).dump(),
                                                      FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
}

const std::vector<MessageItem>& ChatSession::GetHistory() const {
  return history_;
}

void ChatSession::CommitTurn(std::vector<MessageItem>&& new_messages, const Response& response,
                             int pre_turn_token_count, int post_turn_token_count) {
  size_t history_start = history_.size();
  size_t input_count = new_messages.size();

  // Commit input messages to history
  for (auto& msg : new_messages) {
    history_.push_back(std::move(msg));
  }

  // Commit assistant reply from the response (first MESSAGE item with role=assistant)
  for (const auto& item : response.items) {
    if (item->type == FOUNDRY_LOCAL_ITEM_MESSAGE) {
      const auto& msg = static_cast<const MessageItem&>(*item);
      if (msg.role == FOUNDRY_LOCAL_ROLE_ASSISTANT && !msg.content.empty()) {
        history_.push_back(msg);
        break;
      }
    }
  }

  turns_.push_back({history_start, input_count, pre_turn_token_count, post_turn_token_count});
}

size_t ChatSession::TurnCount() const {
  return turns_.size();
}

void ChatSession::UndoTurns(size_t count) {
  if (count == 0) {
    return;
  }

  if (count > turns_.size()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "Cannot undo " + std::to_string(count) + " turns; only " +
                 std::to_string(turns_.size()) + " turns exist");
  }

  // Find the target turn — the one we're rewinding to the start of
  auto& target = turns_[turns_.size() - count];

  // Truncate history back to where the target turn started
  history_.resize(target.history_start);

  // Rewind the generator
  if (cached_generator_) {
    if (count == turns_.size()) {
      // Undoing all turns — destroy the generator entirely
      cached_generator_.reset();
      cached_tool_ctx_ = {};
    } else {
      cached_generator_->RewindTo(target.pre_turn_token_count);
    }
  }

  turns_.resize(turns_.size() - count);
}

size_t ChatSession::MessageCount() const {
  return history_.size();
}
}  // namespace fl
