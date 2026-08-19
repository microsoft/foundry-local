// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/chat/chat_session.h"

#include "contracts/chat_completions.h"
#include "contracts/chat_completions_converter.h"
#include "inferencing/generative/chat/engine_chat_generator.h"
#include "inferencing/generative/chat/onnx_chat_generator.h"
#include "inferencing/generative/chat/reasoning_stream_splitter.h"
#include "inferencing/generative/genai_model_instance.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/generative/toolcalling/tool_call_stream_accumulator.h"
#include "inferencing/generative/toolcalling/tool_call_utils.h"
#include "items/audio_item.h"
#include "items/image_item.h"
#include "items/text_item.h"
#include "items/tool_result_item.h"
#include "items/tool_call_item.h"
#include "model.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <limits>
#include <utility>

namespace fl {

namespace {

template <typename Cleanup>
class ScopeExit final {
 public:
  explicit ScopeExit(Cleanup cleanup) : cleanup_(std::move(cleanup)) {}
  ~ScopeExit() {
    if (active_) {
      cleanup_();
    }
  }

  ScopeExit(const ScopeExit&) = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;

  void Release() noexcept {
    active_ = false;
  }

 private:
  Cleanup cleanup_;
  bool active_{true};
};

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

bool HasMedia(const std::vector<MessageItem>& messages) {
  for (const auto& message : messages) {
    for (const auto& part : message.content) {
      if (part.view &&
          (part.view->type == FOUNDRY_LOCAL_ITEM_IMAGE || part.view->type == FOUNDRY_LOCAL_ITEM_AUDIO)) {
        return true;
      }
    }
  }

  return false;
}

bool RequiresFreshResidentTurn(const ToolCallContext& tool_ctx) {
  const auto has_explicit_guidance =
      !tool_ctx.guidance_type.empty() || !tool_ctx.guidance_data.empty();
  return tool_ctx.HasTools() || has_explicit_guidance || tool_ctx.supports_reasoning;
}

}  // namespace

ChatSession::ChatSession(const fl::Model& catalog_model, GenAIModelInstance& model, ILogger& logger, ITelemetry& telemetry)
    : Session(catalog_model, logger, telemetry), logger_(logger), model_(model) {
  logger_.Log(LogLevel::Debug, fmt::format("Creating ChatSession for model: {}", model.ModelId()));
  // Last so a throw above does not leak a refcount; nothing below can throw.
  model_.AcquireSession();
}

ChatSession::~ChatSession() {
  // Engine requests depend on the loaded model and must close before this session releases its model reference.
  ResetGeneratorCache();

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
      cached_generator_(std::move(other.cached_generator_)),
      resident_generator_(std::move(other.resident_generator_)),
      cached_tool_ctx_(std::move(other.cached_tool_ctx_)),
      cached_options_(std::move(other.cached_options_)) {
  other.owns_session_ = false;
}

SessionType ChatSession::Type() const {
  return SessionType::kChat;
}

void ChatSession::SetSessionOptionsImpl(const KeyValuePairs& options) {
  session_options_ = SearchOptions::FromParameters(options);
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
  if (!tool_choice.has_value()) {
    tool_choice = session_options_.tool_choice;
  }

  if (tool_ctx.HasTools()) {
    ApplyToolChoiceToContext(tool_choice, tool_ctx);
  }

  // Read user-specified guidance from request parameters
  tool_ctx.guidance_type = get_param("guidance_type");
  tool_ctx.guidance_data = get_param("guidance_data");

  return tool_ctx;
}

EngineChatGenerator& ChatSession::PrepareResidentGenerator(
    const std::vector<MessageItem>& all_messages,
    const SearchOptions& options,
    const ToolCallContext& tool_ctx,
    const Request& request) {
  auto cancellation_requested = [&request] {
    return request.canceled.load(std::memory_order_relaxed);
  };

  if (resident_generator_) {
    const auto configuration_matches =
        cached_options_.has_value() && *cached_options_ == options && cached_tool_ctx_ == tool_ctx;

    if (configuration_matches && !RequiresFreshResidentTurn(cached_tool_ctx_) &&
        resident_generator_->IsReadyForContinuation()) {
      const auto prompt_tokens =
          EngineChatGenerator::EncodeMessages(all_messages, Model(), tool_ctx.tools_json);
      if (prompt_tokens.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "chat prompt has too many tokens");
      }

      ResolveMaxOutputTokenLimit(
          options, static_cast<int>(prompt_tokens.size()), Model().GetGenAIConfig());
      if (resident_generator_->TryContinue(prompt_tokens, cancellation_requested)) {
        return *resident_generator_;
      }
    }

    // Configuration changes, non-replayable output, unread state, and prefix mismatches all require full replay.
    ResetGeneratorCache();
  }

  auto stored_options = options;
  auto stored_tool_ctx = tool_ctx;
  auto generator = EngineChatGenerator::Create(
      all_messages, options, Model(), tool_ctx, EngineChatGeneratorMode::kResident,
      std::move(cancellation_requested));

  cached_options_ = std::move(stored_options);
  cached_tool_ctx_ = std::move(stored_tool_ctx);
  resident_generator_ = std::move(generator);
  return *resident_generator_;
}

void ChatSession::ResetGeneratorCache() noexcept {
  resident_generator_.reset();
  cached_generator_.reset();
  cached_options_.reset();
  cached_tool_ctx_ = {};
}

namespace {

using TextSegment = ReasoningStreamSplitter::Segment;

ReasoningStreamSplitter CreateReasoningSplitter(const ToolCallContext& tool_ctx,
                                                GenAIModelInstance& model) {
  if (!tool_ctx.supports_reasoning) {
    return {"", ""};
  }

  auto start = tool_ctx.reasoning_start.empty() ? std::string("<think>") : tool_ctx.reasoning_start;
  auto end = tool_ctx.reasoning_end.empty() ? std::string("</think>") : tool_ctx.reasoning_end;
  auto start_token_ids = model.GetPreprocessor().EncodeTokenIds(start);
  auto end_token_ids = model.GetPreprocessor().EncodeTokenIds(end);
  return {std::move(start), std::move(end), std::move(start_token_ids), std::move(end_token_ids)};
}

void AppendSegment(std::vector<TextSegment>& destination, std::string text, flTextItemType type) {
  if (text.empty()) {
    return;
  }

  if (!destination.empty() && destination.back().type == type) {
    destination.back().text += text;
  } else {
    destination.push_back({std::move(text), type});
  }
}

}  // namespace

void ChatSession::ProcessGeneratedOutput(std::vector<TextSegment> segments,
                                         const SearchOptions& effective_options, bool canceled,
                                         Response& response, int prompt_tokens, int total_tokens,
                                         int reasoning_tokens, std::vector<ParsedToolCall> parsed_calls) {
  int completion_tokens = total_tokens - prompt_tokens;
  const bool has_tool_calls = !parsed_calls.empty();

  if (has_tool_calls) {
    // Add structured tool call items to the response
    auto tool_items = ToolCallsToItems(parsed_calls);
    for (auto& ti : tool_items) {
      response.items.push_back(std::move(ti));
    }
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
  response.usage.reasoning_tokens = reasoning_tokens;

  logger_.Log(LogLevel::Verbose,
              fmt::format(
                  "Completion stats: Total Tokens: {}, Prompt Tokens: {}, Completion Tokens: {}, Reasoning Tokens: {}",
                  total_tokens, prompt_tokens, completion_tokens, reasoning_tokens));
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

  ScopeExit invalidate_on_failure([this] {
    ResetGeneratorCache();
  });

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

  // Media input detection.
  //
  // Media input is only allowed on the first turn of a session. After that:
  //   - AppendMessages (continuous decoding) has no media-aware path; media
  //     parts in subsequent turns would be silently dropped.
  //   - Conversation history can't replay image bytes through the chat
  //     template, so we can't even rebuild from scratch with prior images.
  // The simplest correct behaviour is to require a fresh session for every
  // media request. Text follow-ups within the same session are fine.
  std::vector<const ImageItem*> images;
  std::vector<const AudioItem*> audios;
  for (const auto& msg : new_messages) {
    for (const auto& part : msg.content) {
      if (part.view && part.view->type == FOUNDRY_LOCAL_ITEM_IMAGE) {
        images.push_back(static_cast<const ImageItem*>(part.view));
      } else if (part.view && part.view->type == FOUNDRY_LOCAL_ITEM_AUDIO) {
        audios.push_back(static_cast<const AudioItem*>(part.view));
      }
    }
  }

  const bool media_turn = !images.empty() || !audios.empty();

  if (media_turn && !history_.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "image or audio input is only allowed on the first turn of a session; "
             "create a new ChatSession to send media");
  }

  // Merge session-level and per-request options once for this turn.
  auto effective_kvp = MergedOptions(request.options);
  SearchOptions effective_options = SearchOptions::FromParameters(effective_kvp);
  if (!effective_options.max_output_tokens.has_value()) {
    effective_options.max_output_tokens = media_turn ? 3072 : 2048;
  }

  ChatGenerator* generator = nullptr;
  ToolCallContext turn_tool_ctx;
  int prompt_tokens = 0;
  bool engine_turn = false;
  int max_output = 0;

  try {
    turn_tool_ctx = BuildToolCallContext(request);
    std::vector<MessageItem> all_messages;
    all_messages.reserve(history_.size() + new_messages.size());
    all_messages.insert(all_messages.end(), history_.begin(), history_.end());
    all_messages.insert(all_messages.end(), new_messages.begin(), new_messages.end());

    const auto request_has_media = HasMedia(all_messages);
    engine_turn =
        Model().SupportsEngineChatCompletions() &&
        ShouldUseEngineChatGenerator(Model().GetGenAIConfig(), Model().IsMultiModal(), request_has_media);

    if (engine_turn) {
      cached_generator_.reset();
      generator = &PrepareResidentGenerator(
          all_messages, effective_options, turn_tool_ctx, request);
      prompt_tokens = generator->PromptTokenCount();
    } else {
      if (resident_generator_) {
        ResetGeneratorCache();
      }

      const auto configuration_matches =
          cached_generator_ && cached_options_.has_value() &&
          *cached_options_ == effective_options && cached_tool_ctx_ == turn_tool_ctx;
      if (cached_generator_ && !configuration_matches) {
        ResetGeneratorCache();
      }

      if (cached_generator_) {
        cached_generator_->AppendMessages(new_messages, Model(), cached_tool_ctx_.tools_json);
        prompt_tokens = cached_generator_->TokenCount();
      } else {
        std::unique_ptr<OnnxChatGenerator> new_generator;
        if (media_turn) {
          // Media processing expands the prompt, so keep its allocation bounded to this turn.
          new_generator = OnnxChatGenerator::CreateWithMedia(
              all_messages, effective_options, Model(), images, audios, turn_tool_ctx,
              /*use_full_context=*/false);
        } else {
          new_generator = OnnxChatGenerator::Create(
              all_messages, effective_options, Model(), turn_tool_ctx,
              /*use_full_context=*/true);
        }

        prompt_tokens = new_generator->PromptTokenCount();
        cached_options_ = effective_options;
        cached_tool_ctx_ = turn_tool_ctx;
        cached_generator_ = std::move(new_generator);
      }

      generator = cached_generator_.get();
    }

    max_output = ResolveMaxOutputTokenLimit(
        effective_options, prompt_tokens, Model().GetGenAIConfig());
    turn_tool_ctx = cached_tool_ctx_;
  } catch (...) {
    ResetGeneratorCache();
    throw;
  }

  // Generate token-by-token with optional streaming.
  // Check request.canceled each iteration — a streaming callback returning
  // non-zero sets this flag asynchronously via CallbackHandler.
  auto streaming_callback = CreateCallbackHandler(request);
  int output_tokens = 0;
  std::vector<TextSegment> generated_segments;

  // Marker IDs are derived from the configured strings with the model tokenizer. This detects special markers even
  // when their decoded chunks are empty, while non-reasoning models retain the DEFAULT passthrough.
  auto splitter = CreateReasoningSplitter(turn_tool_ctx, Model());

  // Accumulator: separates visible text from tool-call blocks in the DEFAULT-segment stream. For models without
  // tool-call markers configured, both marker strings are empty and the accumulator degrades to passthrough.
  // REASONING segments bypass the accumulator entirely — tool-call-shaped text inside <think>...</think> is the
  // model's scratchpad and is not a real tool call.
  ToolCallStreamAccumulator tool_accumulator(
      turn_tool_ctx.tool_output ? turn_tool_ctx.tool_call_start : std::string{},
      turn_tool_ctx.tool_output ? turn_tool_ctx.tool_call_end : std::string{});

  // Tool calls parsed during generation are authoritative for the final response too. In particular, an empty result
  // must remain empty rather than re-parsing reasoning text that was intentionally kept away from this accumulator.
  std::vector<ParsedToolCall> streamed_tool_calls;

  auto emit_segments = [&](const std::vector<ReasoningStreamSplitter::Segment>& segments) {
    for (const auto& seg : segments) {
      if (seg.type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING) {
        // REASONING goes straight through — never feed it to the tool-call accumulator.
        AppendSegment(generated_segments, seg.text, seg.type);
        if (streaming_callback) {
          streaming_callback->PushItem(std::make_unique<TextItem>(seg.text, seg.type));
        }
        continue;
      }

      auto out = tool_accumulator.Push(seg.text);

      if (!out.visible_text.empty()) {
        AppendSegment(generated_segments, out.visible_text, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
        if (streaming_callback) {
          streaming_callback->PushItem(
              std::make_unique<TextItem>(std::move(out.visible_text), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT));
        }
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

    if (!out.visible_text.empty()) {
      AppendSegment(generated_segments, out.visible_text, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
      if (streaming_callback) {
        streaming_callback->PushItem(
            std::make_unique<TextItem>(std::move(out.visible_text), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT));
      }
    }

    for (auto& pc : out.ready_calls) {
      if (streaming_callback) {
        streaming_callback->PushItem(std::make_unique<ToolCallItem>(pc.id, pc.name, pc.arguments));
      }
      streamed_tool_calls.push_back(std::move(pc));
    }
  };

  while (!generator->IsDone() && !request.canceled.load(std::memory_order_relaxed)) {
    generator->GenerateNextToken();
    const auto token_id = generator->CurrentTokenId();
    std::string token = generator->Decode();
    output_tokens = generator->TokenCount() - prompt_tokens;

    if (token_id.has_value()) {
      emit_segments(splitter.Push(*token_id, std::move(token)));
    } else if (!token.empty()) {
      emit_segments(splitter.Push(token));
    }

    // Enforce max_output_tokens — with use_full_context the OGA max_length
    // is the entire context window, so we must cap output ourselves.
    if (output_tokens >= max_output) {
      break;
    }
  }

  // End-of-stream: drain the reasoning splitter first so any final DEFAULT bytes feed into the tool accumulator,
  // then drain the tool accumulator.
  emit_segments(splitter.Flush());
  flush_accumulator();

  if (streaming_callback) {
    // Cancellation is decided on the callback worker. Join it before the transactional commit decision.
    streaming_callback->Drain();
  }

  const auto total_tokens = generator->TokenCount();
  const auto canceled = request.canceled.load(std::memory_order_relaxed);
  const auto truncated = output_tokens >= max_output && !generator->IsDone();
  if (canceled || truncated) {
    // A partial Engine turn cannot be continued safely. Replaying committed history is also the safest recovery for
    // the fallback generator after asynchronous cancellation or a caller-enforced output cap.
    ResetGeneratorCache();
  }

  ProcessGeneratedOutput(std::move(generated_segments), effective_options, canceled, response,
                         prompt_tokens, total_tokens, splitter.ReasoningTokenCount(),
                         std::move(streamed_tool_calls));

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
    const auto grammar_was_active = turn_tool_ctx.tool_output && !turn_tool_ctx.text_output;
    const auto reasoning_was_active = turn_tool_ctx.supports_reasoning;

    if (!engine_turn && (grammar_was_active || reasoning_was_active)) {
      ResetGeneratorCache();
    }

    CommitTurn(std::move(new_messages), response);

    // After a media turn, drop the cached generator so any text follow-up
    // rebuilds from history. AppendMessages cannot extend a media-decoded
    // sequence; trying to do so would silently feed text into a state that
    // includes media-derived tokens.
    if (media_turn) {
      ResetGeneratorCache();
    }
  }

  invalidate_on_failure.Release();
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

  // Reconstruct the complete self-contained transcript. BuildRequestItems intentionally uses ToolResultItem for
  // direct sessions, but the chat template also needs prior assistant tool calls and tool results as messages.
  auto messages = chat_completions::BuildPromptMessages(
      req, tool_ctx.tool_call_start, tool_ctx.tool_call_end);

  // JSON requests are always self-contained and stateless, even when the eligible text path uses the shared Engine.
  std::unique_ptr<ChatGenerator> generator;
  const bool request_has_media = HasMedia(messages);
  if (Model().SupportsEngineChatCompletions() &&
      ShouldUseEngineChatGenerator(Model().GetGenAIConfig(), Model().IsMultiModal(), request_has_media)) {
    generator = EngineChatGenerator::Create(
        messages, options, Model(), tool_ctx, EngineChatGeneratorMode::kStateless,
        [&original_request] { return original_request.canceled.load(std::memory_order_relaxed); });
  } else {
    generator = OnnxChatGenerator::Create(messages, options, Model(), tool_ctx);
  }

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
  ToolCallStreamAccumulator tool_accumulator(tool_ctx.tool_output ? tool_ctx.tool_call_start : std::string{},
                                             tool_ctx.tool_output ? tool_ctx.tool_call_end : std::string{});

  // Tool calls parsed during generation are reused by the final response so call IDs stay stable and reasoning text
  // is never reconsidered as a tool call.
  std::vector<ParsedToolCall> streamed_tool_calls;
  std::vector<TextSegment> generated_segments;

  // Chat Completions suppresses REASONING segments, but final response construction still consumes this same typed
  // sequence so it does not need to re-split decoded text.
  auto splitter = CreateReasoningSplitter(tool_ctx, Model());

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
        AppendSegment(generated_segments, seg.text, seg.type);
        continue;
      }

      auto out = tool_accumulator.Push(seg.text);
      AppendSegment(generated_segments, out.visible_text, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
      emit_visible_text(std::move(out.visible_text));
      emit_ready_calls(out.ready_calls);
    }
  };

  // Generate token-by-token.
  try {
    while (!generator->IsDone() && !original_request.canceled) {
      generator->GenerateNextToken();
      const auto token_id = generator->CurrentTokenId();
      std::string token = generator->Decode();

      if (token_id.has_value()) {
        process_segments(splitter.Push(*token_id, std::move(token)));
      } else if (!token.empty()) {
        process_segments(splitter.Push(token));
      }
    }
  } catch (...) {
    generator->Cancel();
    throw;
  }

  if (original_request.canceled) {
    // Engine cancellation is cooperative: an in-flight serialized Step finishes, then Close prevents further work.
    generator->Cancel();
  }

  // Drain any buffered partial-marker bytes at end-of-stream. Reasoning splitter first so any final DEFAULT bytes
  // feed into the tool accumulator; then drain the tool accumulator.
  process_segments(splitter.Flush());
  {
    auto out = tool_accumulator.Flush();
    AppendSegment(generated_segments, out.visible_text, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
    emit_visible_text(std::move(out.visible_text));
    emit_ready_calls(out.ready_calls);
  }

  int total_tokens = generator->TokenCount();

  ProcessGeneratedOutput(std::move(generated_segments), options, original_request.canceled, response,
                         prompt_tokens, total_tokens, splitter.ReasoningTokenCount(),
                         std::move(streamed_tool_calls));

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

void ChatSession::CommitTurn(std::vector<MessageItem>&& new_messages, const Response& response) {
  const auto history_start = history_.size();
  const auto input_count = new_messages.size();

  // Prepare every owning message before mutating history so allocation/copy failures leave the turn uncommitted.
  std::vector<MessageItem> committed_messages;
  committed_messages.reserve(new_messages.size() + 1);
  for (auto& msg : new_messages) {
    committed_messages.push_back(std::move(msg));
  }

  for (const auto& item : response.items) {
    if (item->type == FOUNDRY_LOCAL_ITEM_MESSAGE) {
      const auto& msg = static_cast<const MessageItem&>(*item);
      if (msg.role == FOUNDRY_LOCAL_ROLE_ASSISTANT && !msg.content.empty()) {
        committed_messages.push_back(msg);
        break;
      }
    }
  }

  history_.reserve(history_.size() + committed_messages.size());
  turns_.reserve(turns_.size() + 1);

  for (auto& message : committed_messages) {
    history_.push_back(std::move(message));
  }

  turns_.push_back({history_start, input_count});
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

  const auto& target = turns_[turns_.size() - count];

  // Public Engine continuation has no rewind operation. Closing both cache types also avoids retaining output that
  // no longer corresponds to the committed transcript.
  ResetGeneratorCache();

  history_.resize(target.history_start);
  turns_.resize(turns_.size() - count);
}

size_t ChatSession::MessageCount() const {
  return history_.size();
}
}  // namespace fl
