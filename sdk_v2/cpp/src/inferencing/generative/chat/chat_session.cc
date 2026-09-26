// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/chat/chat_session.h"

#include "contracts/chat_completions.h"
#include "contracts/tool_definitions.h"
#include "contracts/chat_completions_converter.h"
#include "inferencing/execution_provider.h"
#include "inferencing/generative/chat/media_input.h"
#include "inferencing/generative/chat/onnx_chat_engine.h"
#include "inferencing/generative/chat/onnx_chat_generator.h"
#include "inferencing/generative/chat/onnx_engine_chat_stream.h"
#include "inferencing/generative/chat/prepared_chat_prompt.h"
#include "inferencing/generative/chat/reasoning_stream_splitter.h"
#include "inferencing/generative/chat/stop_strings.h"
#include "inferencing/generative/genai_model_instance.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "inferencing/generative/toolcalling/grammar.h"
#include "inferencing/generative/toolcalling/qwen_xml_tool_call_decoder.h"
#include "inferencing/generative/toolcalling/raw_envelope_detector.h"
#include "inferencing/generative/toolcalling/tool_call_stream_accumulator.h"
#include "inferencing/generative/toolcalling/tool_call_utils.h"
#include "inferencing/session/tool_registry.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "model.h"
#include "util/scope_guard.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <optional>
#include <unordered_map>
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

std::unique_ptr<ChatGenerator> CreateTextChatGenerator(const chat_internal::PreparedChatMessages& messages,
                                                       const SearchOptions& options,
                                                       GenAIModelInstance& model,
                                                       const ToolCallContext& tool_ctx,
                                                       bool use_full_context,
                                                       const TextChatGeneratorFactory& factory) {
  if (factory) {
    return factory(messages, options, model, tool_ctx, use_full_context);
  }

  if (model.GetGenAIConfig().GetChatBackendKind() != ChatBackendKind::kGenerator) {
    return OnnxEngineChatStream::Create(messages, options, model, tool_ctx);
  }

  return OnnxChatGenerator::Create(messages, options, model, tool_ctx, use_full_context);
}

std::unique_ptr<ChatGenerator> CreatePreparedTextChatGenerator(
    const chat_internal::PreparedChatMessages& messages,
    PreparedChatPrompt prepared,
    const SearchOptions& options,
    GenAIModelInstance& model,
    const ToolCallContext& tool_ctx,
    bool use_full_context,
    const TextChatGeneratorFactory& factory) {
  if (factory) {
    return factory(messages, options, model, tool_ctx, use_full_context);
  }

  if (model.GetGenAIConfig().GetChatBackendKind() != ChatBackendKind::kGenerator) {
    return OnnxEngineChatStream::CreatePrepared(std::move(prepared), options, model, tool_ctx);
  }

  return OnnxChatGenerator::CreatePrepared(std::move(prepared), options, model, tool_ctx, use_full_context);
}

using TextSegment = ReasoningStreamSplitter::Segment;

/// Value of `key` in `options`, or an empty string when it is absent.
std::string GetOptionOrEmpty(const KeyValuePairs& options, const char* key) {
  auto it = options.find(key);
  return it != options.end() ? it->second : std::string{};
}

/// Use a model-published boundary ID only when its published text matches the configured marker.
std::optional<int32_t> ResolveMarkerTokenId(const std::string& marker_text,
                                            const PublishedMarker& published) {
  if (!UsesPublishedToken(marker_text, published) || *published.id < 0) {
    return std::nullopt;
  }

  return published.id;
}

/// Build the assistant transcript message for a completed generation. The generation events are already in the order
/// the model produced them, so the transcript keeps visible text, reasoning, and calls interleaved exactly as emitted.
///
/// The turn's output has already been streamed to the caller by the time this runs, so a model that emitted argument
/// bytes that are not a JSON object must not fail the request. The raw bytes are kept verbatim on the call (and on
/// the response items), the normalized form degrades to an empty object, and the defect is logged.
///
/// `tool_ctx` supplies the kinds this turn was prompted with, so a custom tool's call is recorded as such and its
/// already-unwrapped text payload is rewrapped for template projection rather than being read as malformed JSON.
TranscriptMessage MakeAssistantMessage(const std::vector<GeneratedOutputEvent>& events, const ToolCallContext& tool_ctx,
                                       ILogger& logger) {
  TranscriptMessage assistant;
  assistant.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;

  for (const auto& event : events) {
    if (const auto* segment = std::get_if<TextSegment>(&event)) {
      if (segment->type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING) {
        assistant.AppendReasoning(segment->text);
      } else {
        assistant.AppendText(segment->text);
      }
      continue;
    }

    const auto& parsed = std::get<ParsedToolCall>(event);
    auto generated = MakeGeneratedToolCall(
        parsed.id, parsed.name, parsed.arguments, tool_ctx.KindOf(parsed.name),
        parsed.raw_envelope ? GeneratedCallEncoding::kRawEnvelope : GeneratedCallEncoding::kStructured);

    if (!generated.arguments_usable) {
      logger.Log(LogLevel::Warning,
                 fmt::format("Model emitted tool call '{}' with arguments that are not a JSON object; keeping the "
                             "raw arguments and treating the call as having none: {}",
                             parsed.name, parsed.arguments));
    }

    assistant.AppendToolCall(std::move(generated.call));
  }

  return assistant;
}

ReasoningStreamSplitter CreateReasoningSplitter(const ToolCallContext& tool_ctx,
                                                GenAIModelInstance& model,
                                                bool prompt_opens_reasoning) {
  if (!tool_ctx.supports_reasoning) {
    return {"", ""};
  }

  auto markers = ResolveReasoningMarkers(tool_ctx, model);
  auto ignored_token_ids = model.GetPreprocessor().GetEosTokenIds();
  return {std::move(markers.start), std::move(markers.end), std::move(markers.start_token_ids),
          std::move(markers.end_token_ids), std::move(ignored_token_ids),
          chat_session_internal::ShouldStartInsideReasoning(tool_ctx, prompt_opens_reasoning)};
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

void AppendGeneratedSegment(std::vector<GeneratedOutputEvent>& destination, std::string text, flTextItemType type) {
  if (text.empty()) {
    return;
  }

  if (!destination.empty()) {
    auto* previous = std::get_if<TextSegment>(&destination.back());
    if (previous && previous->type == type) {
      previous->text += text;
      return;
    }
  }

  destination.push_back(TextSegment{std::move(text), type});
}

/// Report a turn ended at its tool call because the model kept talking afterwards. Both generation paths share the
/// wording so the behaviour reads the same in a log wherever it happened.
void LogTurnEndedAfterToolCall(ILogger& logger, const std::string& dropped_text) {
  logger.Log(LogLevel::Warning,
             fmt::format("Model emitted visible text after a tool call; ending the turn at the call because the "
                         "text cannot be represented in the order it was produced: {}",
                         dropped_text));
}

bool AcceptVisibleText(AssistantTurnGuard& guard, const std::string& text, ILogger& logger) {
  const auto disposition = guard.OfferVisibleText(text);
  if (disposition == TextDisposition::kEndTurn) {
    LogTurnEndedAfterToolCall(logger, text);
  }

  return disposition == TextDisposition::kEmit;
}

void AddSerializedFunctionKinds(
    const nlohmann::json& tools_array,
    std::unordered_map<std::string, ToolKind>& tool_kinds) {
  for (const auto& tool : tools_array) {
    if (!tool.is_object()) {
      continue;
    }

    const nlohmann::json* declaration = &tool;
    const auto function = tool.find("function");
    if (function != tool.end()) {
      const auto type = tool.find("type");
      if (type == tool.end() || !type->is_string() ||
          type->get_ref<const std::string&>() != "function" ||
          !function->is_object()) {
        continue;
      }

      declaration = &*function;
    } else {
      const auto type = tool.find("type");
      if (type != tool.end() &&
          (!type->is_string() ||
           type->get_ref<const std::string&>() != "function")) {
        continue;
      }
    }

    const auto name = declaration->find("name");
    if (name == declaration->end() || !name->is_string()) {
      continue;
    }

    const auto declared_name = name->get<std::string>();
    if (!declared_name.empty()) {
      // Do not overwrite a named custom declaration with compatibility data.
      tool_kinds.emplace(declared_name, ToolKind::kFunction);
    }
  }
}

}  // namespace

namespace chat_session_internal {

std::vector<ToolDefinition> BuildJsonRequestToolDefinitions(
    std::vector<ToolDefinition> definitions, const std::vector<ToolDefinition>& session_snapshot) {
  if (definitions.empty()) {
    return {};
  }

  if (!session_snapshot.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "Tool definitions cannot be used with OpenAI JSON input; the JSON payload must be fully self-contained");
  }

  ToolRegistry request_registry;
  for (auto& definition : definitions) {
    request_registry.Add(std::move(definition));
  }

  return request_registry.Definitions();
}

void PopulateToolDefinitions(const std::vector<ToolDefinition>& definitions, ToolCallContext& context) {
  nlohmann::json tools_array = nlohmann::json::array();
  context.tool_kinds = tools::KindsByName(definitions);

  for (const auto& definition : definitions) {
    if (definition.custom_lark_grammar.has_value()) {
      context.custom_lark_grammars.emplace(definition.name, *definition.custom_lark_grammar);
    }

    if (definition.name.empty()) {
      const auto serialized = nlohmann::json::parse(definition.json_schema);
      if (!serialized.is_array()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                 "an unnamed version 1 tool definition must contain a complete tools array");
      }

      AddSerializedFunctionKinds(serialized, context.tool_kinds);
      for (const auto& tool : serialized) {
        tools_array.push_back(tool);
      }
      continue;
    }

    nlohmann::json tool;
    tool["type"] = "function";
    tool["function"]["name"] = definition.name;

    if (definition.include_description_in_prompt) {
      tool["function"]["description"] = definition.description;
    }

    if (definition.include_parameters_in_prompt && !definition.json_schema.empty()) {
      tool["function"]["parameters"] = nlohmann::json::parse(definition.json_schema);
    }

    if (definition.strict.has_value()) {
      tool["function"]["strict"] = *definition.strict;
    }

    tools_array.push_back(std::move(tool));
  }

  if (!tools_array.empty()) {
    context.tools_json = tools_array.dump();
  }
}

void ResolveBuiltInRawEnvelope(ToolCallContext& context) {
  const auto apply_patch_grammar = context.custom_lark_grammars.find("apply_patch");
  if (apply_patch_grammar == context.custom_lark_grammars.end() ||
      apply_patch_grammar->second != tools::kStockGhcpApplyPatchLarkGrammar) {
    return;
  }

  const RawEnvelopeDescriptor built_in{
      "apply_patch", "*** Begin Patch", "*** End Patch"};
  if (context.raw_envelope.has_value() && *context.raw_envelope != built_in) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "tool_output_encoding conflicts with the stock apply_patch grammar");
  }
  context.raw_envelope = built_in;
}

void ApplyRawEnvelopeGuidance(ToolCallContext& context, ILogger& logger) {
  const auto* raw = context.ActiveRawEnvelope();
  if (raw == nullptr || !context.forced_tool.has_value()) {
    return;
  }

  const auto grammar = context.custom_lark_grammars.find(raw->tool_name);
  const bool has_explicit_guidance = context.HasExplicitGuidance();
  if (grammar != context.custom_lark_grammars.end()) {
    if (has_explicit_guidance &&
        (context.guidance_type != "lark_grammar" || context.guidance_data != grammar->second)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "explicit response guidance conflicts with the forced raw tool grammar");
    }

    context.guidance_type = "lark_grammar";
    context.guidance_data = grammar->second;
    return;
  }

  if (context.HasAnyExplicitGuidance()) {
    logger.Log(LogLevel::Debug,
               fmt::format("Discarding '{}' guidance because forced raw-envelope tool '{}' has no matching grammar",
                           context.guidance_type, raw->tool_name));
  }

  context.guidance_type.clear();
  context.guidance_data.clear();
  context.guidance_disabled = true;
}

bool ShouldStartInsideReasoning(const ToolCallContext& context, bool prompt_opens_reasoning) {
  // Grammar-constrained raw output starts with the envelope itself. An unconstrained raw tool may still reason
  // before emitting its envelope, so it must preserve the prompt-derived reasoning state.
  const bool forced_raw_grammar =
      context.HasForcedRawEnvelope() && context.guidance_type == "lark_grammar" &&
      !context.guidance_data.empty();
  return prompt_opens_reasoning && !forced_raw_grammar;
}

bool ShouldUseQwenXmlToolCallParser(const ToolCallContext& context,
                                    bool has_native_qwen_xml_tool_calls) {
  return has_native_qwen_xml_tool_calls && context.tool_output && context.text_output &&
         !context.forced_tool.has_value() && context.HasTools() &&
         !context.HasAnyExplicitGuidance() &&
         context.tool_call_start == kQwenXmlToolCallStartMarker &&
         context.tool_call_end == kQwenXmlToolCallEndMarker;
}

ToolCallPayloadParser CreateToolCallPayloadParser(
    const ToolCallContext& context, const GenAIModelInstance& model) {
  if (!ShouldUseQwenXmlToolCallParser(
          context, model.HasNativeQwenXmlToolCalls())) {
    return {};
  }

  return CreateQwenXmlToolCallPayloadParser(
      context.tools_json, context.tool_kinds,
      model.GetGenAIConfig().GetChatBackendKind() == ChatBackendKind::kEngine &&
          context.ActiveRawEnvelope() == nullptr);
}

void NormalizeToolOutputBatch(ToolCallStreamAccumulator::Output& output,
                              const ToolCallContext& tool_ctx) {
  for (auto& event : output.events) {
    auto* call = std::get_if<ParsedToolCall>(&event);
    if (call == nullptr) {
      continue;
    }

    if (tool_ctx.IsCustomTool(call->name) && !call->raw_envelope) {
      call->arguments = ExtractCustomToolInput(call->argument_source);
    }

    ValidateToolCallText(call->name, "tool call name");
    ValidateToolCallText(call->arguments, "tool call arguments");
  }
}

namespace {

void AppendToolOutput(ToolCallStreamAccumulator::Output& destination,
                      ToolCallStreamAccumulator::Output source) {
  destination.malformed |= source.malformed;
  destination.events.insert(destination.events.end(),
                            std::make_move_iterator(source.events.begin()),
                            std::make_move_iterator(source.events.end()));
}

bool ContainsToolCall(const ToolCallStreamAccumulator::Output& output) {
  return std::ranges::any_of(output.events, [](const auto& event) {
    return std::holds_alternative<ParsedToolCall>(event);
  });
}

ToolCallStreamAccumulator::Output RouteRawToolOutput(
    RawEnvelopeDetector::Output raw_output,
    RawEnvelopeDetector& raw_detector,
    ToolCallStreamAccumulator& structured_accumulator) {
  ToolCallStreamAccumulator::Output output;
  bool structured_call_completed = false;
  const bool output_contains_raw_call =
      std::ranges::any_of(raw_output.events, [](const auto& event) {
        return std::holds_alternative<ParsedToolCall>(event);
      });
  bool raw_call_completed =
      raw_detector.CallCompleted() && !output_contains_raw_call;

  const auto push_structured = [&](const std::string& text) {
    auto structured_output = structured_accumulator.Push(text);
    structured_call_completed |= ContainsToolCall(structured_output);
    AppendToolOutput(output, std::move(structured_output));

    if (structured_call_completed) {
      auto disabled_output = raw_detector.DisableRecognition();
      for (auto& disabled_event : disabled_output.events) {
        if (auto* disabled_text = std::get_if<std::string>(&disabled_event)) {
          AppendToolOutput(output, structured_accumulator.Push(*disabled_text));
        } else if (auto* rejected = std::get_if<RawEnvelopeDetector::RejectedCandidate>(&disabled_event)) {
          AppendToolOutput(output, structured_accumulator.Push(rejected->text));
        } else {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Disabling raw-envelope recognition unexpectedly produced a call");
        }
      }
    }
  };

  for (auto& event : raw_output.events) {
    if (auto* text = std::get_if<std::string>(&event)) {
      if (raw_call_completed) {
        output.events.emplace_back(std::move(*text));
      } else {
        push_structured(*text);
      }
      continue;
    }

    if (auto* rejected = std::get_if<RawEnvelopeDetector::RejectedCandidate>(&event)) {
      auto structured_output = structured_accumulator.Flush();
      structured_call_completed |= ContainsToolCall(structured_output);
      AppendToolOutput(output, std::move(structured_output));
      output.events.emplace_back(std::move(rejected->text));
      continue;
    }

    // Raw envelopes interrupt the structured byte stream. Release any structured prefix first
    // so caller-visible event ordering remains identical to model output ordering.
    auto structured_output = structured_accumulator.Flush();
    structured_call_completed |= ContainsToolCall(structured_output);
    AppendToolOutput(output, std::move(structured_output));

    auto raw_call = std::move(std::get<ParsedToolCall>(event));
    if (structured_call_completed) {
      AppendToolOutput(output, structured_accumulator.Push(raw_call.arguments));
    } else {
      output.events.emplace_back(std::move(raw_call));
    }
    raw_call_completed = true;
  }

  return output;
}

}  // namespace

ToolCallStreamAccumulator::Output PushToolOutput(
    const std::string& text, RawEnvelopeDetector* raw_detector,
    ToolCallStreamAccumulator& structured_accumulator) {
  if (raw_detector == nullptr) {
    return structured_accumulator.Push(text);
  }

  ToolCallStreamAccumulator::Output output;
  size_t position = 0;
  while (position < text.size()) {
    const auto newline = text.find('\n', position);
    const auto end = newline == std::string::npos ? text.size() : newline + 1;
    const auto part = text.substr(position, end - position);

    if (structured_accumulator.InsideToolCall()) {
      auto structured_output = structured_accumulator.Push(part);
      const bool call_completed = ContainsToolCall(structured_output);
      AppendToolOutput(output, std::move(structured_output));
      if (call_completed) {
        auto disabled = raw_detector->DisableRecognition();
        for (auto& event : disabled.events) {
          if (auto* event_text = std::get_if<std::string>(&event)) {
            output.events.emplace_back(std::move(*event_text));
          } else if (auto* rejected = std::get_if<RawEnvelopeDetector::RejectedCandidate>(&event)) {
            output.events.emplace_back(std::move(rejected->text));
          } else {
            FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Disabling raw-envelope recognition unexpectedly produced a call");
          }
        }
      }
    } else {
      AppendToolOutput(
          output,
          RouteRawToolOutput(raw_detector->Push(part), *raw_detector, structured_accumulator));
    }

    position = end;
  }

  return output;
}

ToolCallStreamAccumulator::Output FlushToolOutput(
    RawEnvelopeDetector* raw_detector,
    ToolCallStreamAccumulator& structured_accumulator,
    bool natural_end) {
  if (raw_detector == nullptr) {
    if (!structured_accumulator.HasPayloadParser()) {
      return structured_accumulator.Flush();
    }

    return natural_end ? structured_accumulator.Flush()
                       : structured_accumulator.RejectPendingSelectedPayload();
  }

  auto raw_output = natural_end ? raw_detector->FinalizeNatural() : raw_detector->Abort();
  auto output = RouteRawToolOutput(std::move(raw_output), *raw_detector, structured_accumulator);
  if (!structured_accumulator.HasPayloadParser()) {
    AppendToolOutput(output, structured_accumulator.Flush());
  } else if (natural_end) {
    AppendToolOutput(output, structured_accumulator.Flush());
  } else {
    AppendToolOutput(output, structured_accumulator.RejectPendingSelectedPayload());
  }
  return output;
}

ToolCallStreamAccumulator::Output AbortRawToolOutput(RawEnvelopeDetector* raw_detector) {
  ToolCallStreamAccumulator::Output output;
  if (raw_detector == nullptr || !raw_detector->HasPendingCandidate()) {
    return output;
  }

  for (auto& event : raw_detector->RejectCandidateForReasoning().events) {
    if (auto* rejected = std::get_if<RawEnvelopeDetector::RejectedCandidate>(&event)) {
      output.events.emplace_back(std::move(rejected->text));
    } else {
      output.events.emplace_back(std::move(std::get<std::string>(event)));
    }
  }

  return output;
}

bool InsideToolOutput(const RawEnvelopeDetector* raw_detector,
                      const ToolCallStreamAccumulator& structured_accumulator) {
  return (raw_detector != nullptr && raw_detector->InsideEnvelope()) ||
         structured_accumulator.InsideToolCall();
}

bool IsNaturalToolOutputEnd(bool canceled,
                            bool stop_sequence_matched,
                            bool host_output_limit_reached,
                            std::optional<BackendTerminationCause> backend_termination) {
  if (canceled || stop_sequence_matched || host_output_limit_reached) {
    return false;
  }

  return backend_termination == BackendTerminationCause::kNaturalEnd;
}

flFinishReason ResolveGeneratedFinishReason(bool has_tool_calls,
                                            bool stop_sequence_matched,
                                            bool host_output_limit_reached,
                                            std::optional<flFinishReason> backend_finish_reason,
                                            int completion_tokens,
                                            std::optional<int> max_output_tokens) {
  if (has_tool_calls) {
    return FOUNDRY_LOCAL_FINISH_TOOL_CALLS;
  }

  if (stop_sequence_matched) {
    return FOUNDRY_LOCAL_FINISH_STOP;
  }

  if (host_output_limit_reached) {
    return FOUNDRY_LOCAL_FINISH_LENGTH;
  }

  if (backend_finish_reason.has_value()) {
    return *backend_finish_reason;
  }

  if (const int max_output = max_output_tokens.value_or(0);
      max_output > 0 && completion_tokens >= max_output) {
    return FOUNDRY_LOCAL_FINISH_LENGTH;
  }

  return FOUNDRY_LOCAL_FINISH_STOP;
}

bool DidHostOutputLimitTruncate(int output_tokens, int max_output_tokens, bool backend_finished) {
  return output_tokens >= max_output_tokens && !backend_finished;
}

bool ShouldEnforceHostOutputLimit(ChatBackendKind backend_kind, bool media_turn) {
  return media_turn || backend_kind == ChatBackendKind::kGenerator;
}

bool ShouldRebuildRetainedGeneratorBeforeAppend(ChatBackendKind backend_kind,
                                                bool guidance_requirement_changed,
                                                bool guidance_payload_changed,
                                                bool retained_generation_settings_changed) {
  (void)backend_kind;
  return guidance_requirement_changed || guidance_payload_changed || retained_generation_settings_changed;
}

bool ShouldInvalidateRetainedGenerationStateAfterSuccessfulTurn(ChatBackendKind backend_kind,
                                                                bool grammar_was_active,
                                                                bool reasoning_was_active,
                                                                bool stop_sequence_matched,
                                                                bool host_output_limit_reached) {
  (void)backend_kind;
  return stop_sequence_matched || host_output_limit_reached || reasoning_was_active || grammar_was_active;
}

bool ShouldInvalidateRetainedGeneratorForUndo(bool undo_all, bool has_pre_turn_boundary, bool can_rewind) {
  return undo_all || !has_pre_turn_boundary || !can_rewind;
}

}  // namespace chat_session_internal

namespace {

struct GuidedEngineRetryResult {
  std::vector<GeneratedOutputEvent> events;
  int prompt_tokens = 0;
  int total_tokens = 0;
  int reasoning_tokens = 0;
  int cached_prompt_tokens = 0;
  std::optional<flFinishReason> finish_reason;
  bool canceled = false;
};

std::vector<ParsedToolCall> ParseStrictGuidedToolCalls(
    const std::string& text,
    const ToolCallContext& tool_ctx) {
  const auto first = text.find_first_not_of(" \t\r\n");
  const auto last = text.find_last_not_of(" \t\r\n");
  if (first == std::string::npos || last == std::string::npos) {
    return {};
  }

  const auto trimmed = std::string_view(text).substr(first, last - first + 1);
  if (trimmed.size() < tool_ctx.tool_call_start.size() + tool_ctx.tool_call_end.size() ||
      !trimmed.starts_with(tool_ctx.tool_call_start) ||
      !trimmed.ends_with(tool_ctx.tool_call_end)) {
    return {};
  }

  const auto payload = trimmed.substr(
      tool_ctx.tool_call_start.size(),
      trimmed.size() - tool_ctx.tool_call_start.size() - tool_ctx.tool_call_end.size());
  return ParseQwenGuidedToolCalls(payload, tool_ctx.tools_json, tool_ctx.tool_kinds);
}

GuidedEngineRetryResult RunGuidedEngineToolRetry(
    const chat_internal::PreparedChatMessages& messages,
    const SearchOptions& options,
    GenAIModelInstance& model,
    ToolCallContext tool_ctx,
    const Request& request,
    bool use_full_context,
    const TextChatGeneratorFactory& factory) {
  tool_ctx.text_output = false;
  tool_ctx.tool_output = true;

  auto generator =
      CreateTextChatGenerator(messages, options, model, tool_ctx, use_full_context, factory);
  auto splitter = CreateReasoningSplitter(tool_ctx, model, generator->PromptOpensReasoning());

  GuidedEngineRetryResult result;
  std::string structured_output;
  auto process_segments = [&](const std::vector<TextSegment>& segments) {
    for (const auto& segment : segments) {
      if (segment.type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT) {
        structured_output += segment.text;
      } else {
        AppendGeneratedSegment(result.events, segment.text, segment.type);
      }
    }
  };

  StopStringFilter stop_filter(options.stop_sequences);
  auto* active_stop_filter = options.stop_sequences.empty() ? nullptr : &stop_filter;
  bool stop_sequence_matched = false;
  while (!generator->IsDone() && !request.IsCancellationRequested()) {
    generator->GenerateNextToken();
    const auto token_id = generator->CurrentTokenId();
    auto token = generator->Decode();
    if (chat_session_internal::PushDecodedFragment(
            token, token_id, active_stop_filter, splitter, process_segments)) {
      stop_sequence_matched = true;
      break;
    }
  }

  result.canceled = request.IsCancellationRequested();
  if (result.canceled) {
    generator->Cancel();
  }

  chat_session_internal::FlushDecodedStream(active_stop_filter, splitter, process_segments);

  if (!result.canceled && request.IsCancellationRequested()) {
    result.canceled = true;
    generator->Cancel();
  } else if (stop_sequence_matched && !generator->IsDone()) {
    generator->Cancel();
  }

  std::optional<BackendTerminationCause> termination;
  result.prompt_tokens = generator->PromptTokenCount();
  result.total_tokens = generator->TokenCount();
  if (const auto usage = generator->GetTurnUsage()) {
    result.prompt_tokens = usage->prompt_tokens;
    result.total_tokens = usage->prompt_tokens + usage->generated_tokens;
    result.cached_prompt_tokens = usage->cached_prompt_tokens;
    result.finish_reason = usage->finish_reason;
    termination = usage->termination_cause;
  }

  const bool canceled_after_usage = result.canceled || request.IsCancellationRequested();
  if (!result.canceled && canceled_after_usage) {
    generator->Cancel();
  }
  result.canceled = canceled_after_usage;
  if (result.canceled) {
    result.events.clear();
    return result;
  }

  const bool natural_end = chat_session_internal::IsNaturalToolOutputEnd(
      /*canceled=*/false, stop_sequence_matched,
      /*host_output_limit_reached=*/false, termination);
  result.reasoning_tokens = splitter.ReasoningTokenCount();

  auto calls = ParseStrictGuidedToolCalls(structured_output, tool_ctx);
  if (!natural_end || calls.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "Model emitted a malformed tool call and guided recovery did not produce a valid tool call");
  }

  for (auto& call : calls) {
    ToolCallStreamAccumulator::Output output;
    output.events.emplace_back(std::move(call));
    chat_session_internal::NormalizeToolOutputBatch(output, tool_ctx);
    result.events.emplace_back(
        std::move(std::get<ParsedToolCall>(output.events.front())));
  }

  return result;
}

bool HasSemanticOutput(const ToolCallStreamAccumulator::Output& output) {
  return std::ranges::any_of(output.events, [](const auto& event) {
    if (const auto* text = std::get_if<std::string>(&event)) {
      return !text->empty();
    }

    return true;
  });
}

[[noreturn]] void ThrowMalformedToolCallRecoveryFailure() {
  FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
           "Model emitted a malformed tool call and guided recovery did not produce a valid tool call");
}

bool IsGuidedEngineToolRetryEligible(
    ChatBackendKind backend_kind,
    bool natural_tool_output_end,
    bool semantic_output_seen,
    const SearchOptions& options,
    const ToolCallContext& tool_ctx) {
  return backend_kind == ChatBackendKind::kEngine && natural_tool_output_end &&
         !semantic_output_seen && options.stop_sequences.empty() &&
         tool_ctx.text_output && tool_ctx.tool_output &&
         !tool_ctx.forced_tool.has_value() &&
         tool_ctx.guidance_type.empty() && tool_ctx.guidance_data.empty() &&
         tool_ctx.ActiveRawEnvelope() == nullptr;
}

}  // namespace

ChatSession::ChatSession(const fl::Model& catalog_model, GenAIModelInstance& model, ILogger& logger,
                         ITelemetry& telemetry, ChatTranscript::CommitFaultInjector transcript_fault_injector,
                         TextChatGeneratorFactory text_generator_factory,
                         ChatMessagePreparer message_preparer)
    : Session(catalog_model, logger, telemetry),
      logger_(logger),
      model_(model),
      transcript_(std::move(transcript_fault_injector)),
      text_generator_factory_(std::move(text_generator_factory)),
      message_preparer_(message_preparer ? std::move(message_preparer)
                                         : ChatMessagePreparer(chat_internal::PrepareChatMessages)) {
  logger_.Log(LogLevel::Debug, fmt::format("Creating ChatSession for model: {}", model.ModelId()));
  // Last so a throw above does not leak a refcount; nothing below can throw.
  model_.AcquireSession();
}

ChatSession::~ChatSession() {
  if (owns_session_) {
    // Engine streams must close their model-owned conversation before the session releases the model.
    cached_generator_.reset();
    model_.ReleaseSession();
  }
}

ChatSession::ChatSession(ChatSession&& other) noexcept
    : Session(std::move(other)),
      logger_(other.logger_),
      model_(other.model_),
      owns_session_(other.owns_session_),
      transcript_(std::move(other.transcript_)),
      session_options_(std::move(other.session_options_)),
      cached_generator_(std::move(other.cached_generator_)),
      cached_tool_ctx_(std::move(other.cached_tool_ctx_)),
      cached_search_options_(std::move(other.cached_search_options_)),
      system_prompt_(std::move(other.system_prompt_)),
      text_generator_factory_(std::move(other.text_generator_factory_)),
      message_preparer_(std::move(other.message_preparer_)) {
  other.owns_session_ = false;
}

SessionType ChatSession::Type() const {
  return SessionType::kChat;
}

std::string ChatSession::ExecutionProvider() const {
  return std::string(EPUtils::EPtoTelemetryName(model_.EP(), model_.GetGenAIConfig().DefaultProvider()));
}

void ChatSession::SetSessionOptionsImpl(const KeyValuePairs& options) {
  session_options_ = SearchOptions::FromParameters(options);
}

namespace {

ToolCallContext BuildToolCallContextForRequest(const Request& request,
                                               const std::vector<ToolDefinition>& definitions,
                                               const SearchOptions& session_options,
                                               const ModelInfo& model_info,
                                               GenAIModelInstance& model,
                                               ILogger& logger) {
  ToolCallContext tool_ctx;

  tool_ctx.tool_call_start = GetOptionOrEmpty(request.options, FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_START_STR);
  tool_ctx.tool_call_end = GetOptionOrEmpty(request.options, FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_END_STR);
  tool_ctx.template_kwargs_json = GetOptionOrEmpty(request.options, "chat_template_kwargs");
  if (!tool_ctx.template_kwargs_json.empty()) {
    tool_ctx.template_kwargs_json = NormalizeChatTemplateKwargs(tool_ctx.template_kwargs_json);
    const auto kwargs = nlohmann::json::parse(tool_ctx.template_kwargs_json);
    const bool uses_reasoning_controls = kwargs.contains("enable_thinking") ||
                                         kwargs.contains("preserve_thinking") ||
                                         kwargs.contains("reasoning_effort");
    if (uses_reasoning_controls && model.ModelType() == "qwen3_5_text" &&
        !model.SupportsReasoningControls()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
               "the loaded runtime cannot render this model's reasoning controls; use a GenAI package "
               "containing ORT Extensions Jinja identity-predicate support");
    }
  }
  tool_ctx.supports_reasoning_history = model.SupportsReasoningHistory();

  // Fall back to model info properties if not specified in the request
  const auto& info = model_info;

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

  // Catalog metadata is immutable and may not contain markers for models whose
  // tokenizer defines them dynamically. Read those markers from the loaded GenAI
  // model without mutating the published ModelInfo.
  const auto& tag_info = model.GetTagInfo();
  if (tool_ctx.tool_call_start.empty()) {
    tool_ctx.tool_call_start = tag_info.bot_str;
  }
  if (tool_ctx.tool_call_end.empty()) {
    tool_ctx.tool_call_end = tag_info.eot_str;
  }

  // Resolve each boundary independently. A matching model-published ID uses llguidance's exact numeric token syntax;
  // an override or a boundary without an authoritative ID is rendered as a quoted literal.
  if (tool_ctx.HasToolCallTokens()) {
    tool_ctx.tool_call_start_token_id =
        ResolveMarkerTokenId(tool_ctx.tool_call_start, {tag_info.bot_id, tag_info.bot_str});
    tool_ctx.tool_call_end_token_id =
        ResolveMarkerTokenId(tool_ctx.tool_call_end, {tag_info.eot_id, tag_info.eot_str});
  }

  // Check if the model supports chain-of-thought reasoning
  const auto* reasoning_val = info.GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT);
  if (reasoning_val && *reasoning_val == 1) {
    tool_ctx.supports_reasoning = true;
  }

  // Read reasoning marker tokens — same pattern as tool_call tokens
  tool_ctx.reasoning_start = GetOptionOrEmpty(request.options, FOUNDRY_LOCAL_MODEL_PROP_REASONING_START_STR);
  tool_ctx.reasoning_end = GetOptionOrEmpty(request.options, FOUNDRY_LOCAL_MODEL_PROP_REASONING_END_STR);

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
  if (tool_ctx.reasoning_start.empty()) {
    tool_ctx.reasoning_start = tag_info.bor_str;
  }
  if (tool_ctx.reasoning_end.empty()) {
    tool_ctx.reasoning_end = tag_info.eor_str;
  }

  // Resolve reasoning boundaries independently from tool-call boundaries.
  if (tool_ctx.HasReasoningTokens()) {
    tool_ctx.reasoning_start_token_id =
        ResolveMarkerTokenId(tool_ctx.reasoning_start, {tag_info.bor_id, tag_info.bor_str});
    tool_ctx.reasoning_end_token_id =
        ResolveMarkerTokenId(tool_ctx.reasoning_end, {tag_info.eor_id, tag_info.eor_str});
  }

  // Serialize named definitions into the OpenAI tools array the chat template expects. Released
  // version 1 C ABI callers instead supply one unnamed definition whose schema is already a
  // complete tools array; preserve that representation and append its elements in registration
  // order when old and new callers are mixed.
  // Custom tools are already normalized by the registry into a function-shaped schema, so the
  // template and the guidance grammar only ever see function tools. Their kinds are carried on the
  // context, so this turn's output is read back with exactly the tool set that shaped its prompt
  // even if the session's registry changes underneath.
  chat_session_internal::PopulateToolDefinitions(definitions, tool_ctx);
  tool_ctx.forced_tool = request.forced_tool_choice;
  tool_ctx.raw_envelope = request.raw_envelope_descriptor;
  chat_session_internal::ResolveBuiltInRawEnvelope(tool_ctx);

  // Determine text_output / tool_output from tool_choice parameter.
  // ParseToolChoice rejects unknown values with FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT.
  auto tool_choice = SearchOptions::ParseToolChoice(request.options);
  if (!tool_choice.has_value()) {
    tool_choice = session_options.tool_choice;
  }

  // User guidance is independent of generated tool guidance and must remain authoritative even when a malformed
  // legacy tool schema makes automatic tool guidance unavailable.
  tool_ctx.guidance_type = GetOptionOrEmpty(request.options, "guidance_type");
  tool_ctx.guidance_data = GetOptionOrEmpty(request.options, "guidance_data");
  if (tool_ctx.HasPartialExplicitGuidance()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "guidance_type and guidance_data must be provided together");
  }

  if (tool_ctx.HasTools()) {
    ApplyToolChoiceToContext(tool_choice, tool_ctx);

    // Preserve legacy serialized definitions in the prompt, but never let malformed schema data reach generated
    // guidance. Decoder-specific eligibility is checked only when selecting that decoder.
    if (tool_ctx.tool_output && BuildToolJsonSchema(tool_ctx) == "{}") {
      if (!tool_ctx.text_output || tool_ctx.forced_tool.has_value()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                 "tool-only output requires valid tool definitions");
      }

      if (!tool_ctx.HasExplicitGuidance()) {
        tool_ctx.guidance_disabled = true;
      }
    }
  }

  chat_session_internal::ApplyRawEnvelopeGuidance(tool_ctx, logger);

  return tool_ctx;
}

}  // namespace

struct PreparedChatRequest {
  TranscriptIngest ingest;
  MediaInput media;
  ToolCallContext tool_context;
  SearchOptions options;
  ChatBackendKind backend_kind = ChatBackendKind::kGenerator;
  std::string system_prompt;
  std::optional<chat_internal::PreparedChatMessages> messages;
  PreparedChatPrompt prompt;
  std::optional<int> host_max_output_tokens;
  std::string json_model_name;
  bool json_passthrough = false;
};

namespace {

void ResolvePreparedGenerationLimit(PreparedChatRequest& prepared, bool media_turn) {
  if (chat_session_internal::ShouldEnforceHostOutputLimit(prepared.backend_kind, media_turn)) {
    prepared.host_max_output_tokens =
        ResolveMaxOutputTokens(prepared.options, GetDefaultMaxOutputTokens(media_turn));
  }
}

std::unique_ptr<PreparedChatRequest> PrepareChatRequest(
    const Request& request,
    const ChatTranscript& transcript,
    const KeyValuePairs& base_session_options,
    const SearchOptions& chat_session_options,
    const std::vector<ToolDefinition>& tool_definitions,
    const ModelInfo& model_info,
    GenAIModelInstance& model,
    ILogger& logger,
    const ChatMessagePreparer& message_preparer) {
  auto prepared = std::make_unique<PreparedChatRequest>();

  for (const auto* item : request.items) {
    if (item->type != FOUNDRY_LOCAL_ITEM_TEXT) {
      continue;
    }

    const auto& text_item = static_cast<const TextItem&>(*item);
    if (text_item.text_type != FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON) {
      continue;
    }

    auto request_json = nlohmann::json::parse(text_item.text);
    auto chat_request = request_json.get<ChatCompletionRequest>();
    chat_completions::ApplyCatalogDefaults(chat_request, model_info.model_settings);
    prepared->json_model_name = chat_request.model;

    Request internal_request;
    internal_request.forced_tool_choice = request.forced_tool_choice;
    internal_request.raw_envelope_descriptor = request.raw_envelope_descriptor;
    chat_completions::BuildRequestItems(chat_request, internal_request);
    if (internal_request.items.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "the request has nothing to generate from: `messages` carried no content");
    }

    auto definitions = request.prepared_tool_definitions.has_value()
                           ? *request.prepared_tool_definitions
                           : chat_completions::ExtractToolDefinitions(chat_request, internal_request);
    chat_completions::MapRequestParameters(chat_request, internal_request);
    chat_completions::MapGuidance(chat_request, internal_request);
    chat_completions::MapStopSequences(chat_request, internal_request);

    internal_request.options = MergeKeyValuePairs(request.options, internal_request.options);

    std::vector<ToolDefinition> request_definitions;
    if (request.prepared_tool_definitions.has_value()) {
      if (!tool_definitions.empty()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                 "Tool definitions cannot be used with OpenAI JSON input; the JSON payload must be fully "
                 "self-contained");
      }

      request_definitions = std::move(definitions);
    } else {
      request_definitions =
          chat_session_internal::BuildJsonRequestToolDefinitions(std::move(definitions), tool_definitions);
    }

    if (!internal_request.raw_envelope_descriptor.has_value() && chat_request.metadata.has_value()) {
      const auto descriptor = chat_request.metadata->find(tools::kRawEnvelopeMetadataKey);
      if (descriptor != chat_request.metadata->end()) {
        internal_request.raw_envelope_descriptor = tools::ParseRawEnvelopeDescriptor(descriptor->second);
      }
    }
    if (internal_request.raw_envelope_descriptor.has_value()) {
      tools::ValidateRawEnvelopeTool(*internal_request.raw_envelope_descriptor, request_definitions);
    }

    prepared->tool_context = BuildToolCallContextForRequest(internal_request, request_definitions,
                                                            chat_session_options, model_info, model, logger);
    prepared->options =
        SearchOptions::FromParameters(MergeKeyValuePairs(base_session_options, internal_request.options));
    prepared->backend_kind = model.GetGenAIConfig().GetChatBackendKind();

    auto messages = BuildTranscriptMessages(internal_request.items, prepared->tool_context.tool_kinds);
    const ChatTranscript payload_transcript;
    payload_transcript.ValidateInputs(messages);
    prepared->messages.emplace(message_preparer(std::move(messages), model.HasPositionalToolResults()));
    prepared->prompt = PrepareTextChatPrompt(*prepared->messages, model, prepared->tool_context);
    prepared->json_passthrough = true;
    ResolvePreparedGenerationLimit(*prepared, /*media_turn=*/false);
    return prepared;
  }

  prepared->tool_context = BuildToolCallContextForRequest(request, tool_definitions,
                                                          chat_session_options, model_info, model, logger);
  prepared->ingest = IngestRequestItems(request.items, request.item_segment_starts,
                                        prepared->tool_context.tool_kinds);
  transcript.ValidateInputs(prepared->ingest.messages);
  prepared->media = CollectMediaInput(request);
  const bool media_turn = !prepared->media.Empty();
  ValidateMediaTurn(prepared->media, prepared->ingest.messages,
                    {.session_has_history = !transcript.Empty(),
                     .tools_declared = prepared->tool_context.HasTools()});

  const auto effective_kvp = MergeKeyValuePairs(base_session_options, request.options);
  prepared->options = SearchOptions::FromParameters(effective_kvp);
  prepared->backend_kind = model.GetGenAIConfig().GetChatBackendKind();
  prepared->system_prompt = GetOptionOrEmpty(effective_kvp, kSystemPromptOption);

  if (!TurnCanGenerate(prepared->ingest.messages,
                       {.media = media_turn,
                        .history = !transcript.Empty(),
                        .system_prefix = !prepared->system_prompt.empty()})) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "the request has nothing to generate from: no message content, no image or audio, no instructions, "
             "and no conversation to continue");
  }

  std::vector<TranscriptMessage> all_messages;
  const auto& committed = transcript.Messages();
  all_messages.reserve(committed.size() + prepared->ingest.messages.size() +
                       (prepared->system_prompt.empty() ? 0u : 1u));
  all_messages.insert(all_messages.end(), committed.begin(), committed.end());
  all_messages.insert(all_messages.end(), prepared->ingest.messages.begin(), prepared->ingest.messages.end());
  all_messages = WithSystemPrompt(prepared->system_prompt, std::move(all_messages));
  prepared->messages.emplace(message_preparer(std::move(all_messages), model.HasPositionalToolResults()));

  if (media_turn) {
    auto media_messages = std::move(prepared->media.messages);
    if (!prepared->system_prompt.empty()) {
      media_messages.insert(media_messages.begin(),
                            MessageItem(FOUNDRY_LOCAL_ROLE_SYSTEM, prepared->system_prompt));
    }

    prepared->prompt =
        PrepareMediaChatPrompt(media_messages, model, prepared->media.images, prepared->media.audios,
                               prepared->tool_context);
  } else {
    prepared->prompt = PrepareTextChatPrompt(*prepared->messages, model, prepared->tool_context);
  }

  ResolvePreparedGenerationLimit(*prepared, media_turn);
  return prepared;
}

class ChatRequestPreflightOperation final : public Session::RequestPreflightOperation {
 public:
  ChatRequestPreflightOperation(Request request,
                                ChatTranscript transcript,
                                KeyValuePairs base_session_options,
                                SearchOptions chat_session_options,
                                std::vector<ToolDefinition> tool_definitions,
                                ModelInfo model_info,
                                GenAIModelInstance& model,
                                ILogger& logger,
                                ChatMessagePreparer message_preparer)
      : request_(std::move(request)),
        transcript_(std::move(transcript)),
        base_session_options_(std::move(base_session_options)),
        chat_session_options_(std::move(chat_session_options)),
        tool_definitions_(std::move(tool_definitions)),
        model_info_(std::move(model_info)),
        model_(model),
        logger_(logger),
        message_preparer_(std::move(message_preparer)) {
    model_.AcquireSession();
  }

  ~ChatRequestPreflightOperation() override {
    model_.ReleaseSession();
  }

  RequestBudget Execute() override {
    if (!request_.has_value()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "request preflight operation has already been executed");
    }

    auto request = std::move(*request_);
    request_.reset();
    auto prepared = PrepareChatRequest(request, transcript_, base_session_options_, chat_session_options_,
                                       tool_definitions_, model_info_, model_, logger_, message_preparer_);
    auto context_limit = static_cast<int64_t>(GetModelMaxContextLength(model_.GetGenAIConfig()));
    if (prepared->backend_kind == ChatBackendKind::kEngine && prepared->media.Empty()) {
      const auto* engine = model_.GetChatEngine();
      if (!engine) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Engine request preflight requires a loaded chat engine");
      }
      if (engine->MaxRequestLength() > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Engine max_request_length exceeds the preflight result range");
      }
      context_limit = static_cast<int64_t>(engine->MaxRequestLength());
    }

    const auto output_reserve =
        ResolveOutputReserve(prepared->options, prepared->backend_kind, !prepared->media.Empty(),
                             prepared->prompt.prompt_token_count, context_limit);
    return ComputeRequestBudget(prepared->prompt.prompt_token_count, output_reserve, context_limit);
  }

 private:
  std::optional<Request> request_;
  ChatTranscript transcript_;
  KeyValuePairs base_session_options_;
  SearchOptions chat_session_options_;
  std::vector<ToolDefinition> tool_definitions_;
  ModelInfo model_info_;
  GenAIModelInstance& model_;
  ILogger& logger_;
  ChatMessagePreparer message_preparer_;
};

}  // namespace

std::unique_ptr<Session::RequestPreflightOperation> ChatSession::CreateRequestPreflightImpl(Request request) const {
  return std::make_unique<ChatRequestPreflightOperation>(
      std::move(request), transcript_, SessionOptions(), session_options_, ToolDefinitions(), CatalogModel().Info(),
      model_, logger_, message_preparer_);
}

void ChatSession::ProcessGeneratedOutput(std::vector<GeneratedOutputEvent> events,
                                         const ToolCallContext& tool_ctx,
                                         const SearchOptions& effective_options,
                                         bool stop_sequence_matched,
                                         bool host_output_limit_reached,
                                         Response& response,
                                         int prompt_tokens,
                                         int total_tokens,
                                         int reasoning_tokens,
                                         int cached_prompt_tokens,
                                         std::optional<flFinishReason> backend_finish_reason) {
  int completion_tokens = total_tokens - prompt_tokens;
  bool has_tool_calls = false;
  std::vector<TextSegment> segments;

  auto flush_segments = [&]() {
    if (segments.empty()) {
      return;
    }

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
    segments.clear();
  };

  for (auto& event : events) {
    if (auto* segment = std::get_if<TextSegment>(&event)) {
      AppendSegment(segments, std::move(segment->text), segment->type);
      continue;
    }

    flush_segments();
    auto& call = std::get<ParsedToolCall>(event);
    const auto kind = tool_ctx.KindOf(call.name);
    response.items.push_back(std::make_unique<ToolCallItem>(std::move(call.id), std::move(call.name),
                                                            std::move(call.arguments),
                                                            /*replayed_from_store=*/false, kind, std::nullopt,
                                                            call.raw_envelope
                                                                ? GeneratedCallEncoding::kRawEnvelope
                                                                : GeneratedCallEncoding::kStructured));
    has_tool_calls = true;
  }
  flush_segments();

  response.finish_reason = chat_session_internal::ResolveGeneratedFinishReason(
      has_tool_calls, stop_sequence_matched, host_output_limit_reached, backend_finish_reason, completion_tokens,
      effective_options.max_output_tokens);

  response.usage.prompt_tokens = prompt_tokens;
  response.usage.completion_tokens = completion_tokens;
  response.usage.total_tokens = total_tokens;
  response.usage.reasoning_tokens = reasoning_tokens;
  response.usage.cached_prompt_tokens = cached_prompt_tokens;

  logger_.Log(LogLevel::Verbose,
              fmt::format(
                  "Completion stats: Total Tokens: {}, Prompt Tokens: {}, Completion Tokens: {}, Reasoning Tokens: {}",
                  total_tokens, prompt_tokens, completion_tokens, reasoning_tokens));
}

void ChatSession::ProcessRequestImpl(const Request& request, Response& response) {
  auto prepared = PrepareChatRequest(request, transcript_, SessionOptions(), session_options_, ToolDefinitions(),
                                     CatalogModel().Info(), Model(), logger_, message_preparer_);

  if (prepared->json_passthrough) {
    ProcessChatCompletionsJson(*prepared, request, response);
    return;
  }

  auto turn_tool_ctx = prepared->tool_context;
  auto& ingest = prepared->ingest;
  auto inputs = std::move(ingest.messages);
  auto& media = prepared->media;
  const bool media_turn = !media.Empty();
  const auto& effective_options = prepared->options;
  const auto backend_kind = prepared->backend_kind;
  const auto& turn_system_prompt = prepared->system_prompt;
  auto& prepared_messages = *prepared->messages;

  int prompt_tokens = 0;
  // Empty until this turn's input is appended to an existing generator. A rebuilt generator bakes the input into its
  // prompt, so there is no pre-turn boundary that undo could rewind back to.
  std::optional<int> pre_turn_token_count;

  // Classic Generator cannot append tool exchanges or a changed prefix in isolation. Engine always renders the full
  // transcript and performs token-prefix reconciliation, so it can decide safely whether to reuse or replace state.
  const bool retained_tool_context_changed =
      cached_tool_ctx_.supports_tool_calling != turn_tool_ctx.supports_tool_calling ||
      cached_tool_ctx_.tool_call_start != turn_tool_ctx.tool_call_start ||
      cached_tool_ctx_.tool_call_end != turn_tool_ctx.tool_call_end ||
      cached_tool_ctx_.supports_reasoning != turn_tool_ctx.supports_reasoning ||
      cached_tool_ctx_.reasoning_start != turn_tool_ctx.reasoning_start ||
      cached_tool_ctx_.reasoning_end != turn_tool_ctx.reasoning_end ||
      cached_tool_ctx_.template_kwargs_json != turn_tool_ctx.template_kwargs_json ||
      !cached_tool_ctx_.HasSameTools(turn_tool_ctx);

  if (cached_generator_ &&
      (turn_system_prompt != system_prompt_ || retained_tool_context_changed ||
       (backend_kind == ChatBackendKind::kGenerator && (inputs.empty() || CarriesToolActivity(inputs))))) {
    InvalidateCachedGenerator();
  }

  if (cached_generator_) {
    auto guidance_ctx = turn_tool_ctx;
    const bool prev_has_user_guidance = cached_tool_ctx_.HasExplicitGuidance();
    const bool curr_has_user_guidance = guidance_ctx.HasExplicitGuidance();
    const bool prev_needs_guidance =
        prev_has_user_guidance || (cached_tool_ctx_.tool_output && !cached_tool_ctx_.text_output);
    const bool curr_needs_guidance =
        curr_has_user_guidance || (guidance_ctx.tool_output && !guidance_ctx.text_output);
    const bool guidance_payload_changed =
        cached_tool_ctx_.guidance_type != guidance_ctx.guidance_type ||
        cached_tool_ctx_.guidance_data != guidance_ctx.guidance_data;
    const bool options_are_request_baked = backend_kind == ChatBackendKind::kGenerator;

    if (chat_session_internal::ShouldRebuildRetainedGeneratorBeforeAppend(
            backend_kind,
            options_are_request_baked &&
                (prev_needs_guidance != curr_needs_guidance || prev_has_user_guidance),
            options_are_request_baked && guidance_payload_changed,
            !cached_search_options_.HasSameRetainedGenerationSettings(effective_options, backend_kind))) {
      InvalidateCachedGenerator();
    }
  }

  // Past this point the cached generator gets this turn's input appended or is created for it. Any exit before the
  // commit below leaves its KV cache holding tokens the transcript knows nothing about, so the guard drops it and the
  // next turn rebuilds deterministically from committed history. Cancellation of an appended turn is the one settled
  // case that can be rewound instead, and dismisses the guard explicitly below.
  bool turn_committed = false;
  ScopeGuard invalidate_uncommitted([&]() noexcept {
    if (!turn_committed) {
      InvalidateCachedGenerator();
    }
  });

  if (cached_generator_) {
    pre_turn_token_count = cached_generator_->TokenCount();
    try {
      if (text_generator_factory_) {
        cached_generator_->AppendMessages(inputs, prepared_messages, Model(), turn_tool_ctx, effective_options);
      } else {
        cached_generator_->AppendPreparedPrompt(inputs, prepared->prompt, Model(), turn_tool_ctx, effective_options);
      }
      prompt_tokens = cached_generator_->TokenCount();
      cached_tool_ctx_ = turn_tool_ctx;
    } catch (const RetainedPromptMismatchError&) {
      InvalidateCachedGenerator();
      pre_turn_token_count.reset();
    } catch (const OnnxChatEngine::ConversationEvictedError&) {
      InvalidateCachedGenerator();
      pre_turn_token_count.reset();
    }
  }

  if (!cached_generator_) {
    auto tool_ctx = std::move(turn_tool_ctx);

    std::unique_ptr<ChatGenerator> generator;
    if (media_turn) {
      generator = OnnxChatGenerator::CreatePrepared(std::move(prepared->prompt), effective_options, Model(), tool_ctx,
                                                    /*use_full_context=*/false);
    } else {
      generator = CreatePreparedTextChatGenerator(prepared_messages, std::move(prepared->prompt), effective_options,
                                                  Model(), tool_ctx, /*use_full_context=*/true,
                                                  text_generator_factory_);
    }

    prompt_tokens = generator->PromptTokenCount();
    cached_generator_ = std::move(generator);
    cached_tool_ctx_ = std::move(tool_ctx);
    cached_search_options_ = effective_options;
    system_prompt_ = turn_system_prompt;
  }

  const auto host_max_output = prepared->host_max_output_tokens;

  // Generate token-by-token with optional streaming.
  // Check request cancellation each iteration — a streaming callback returning
  // non-zero sets this flag asynchronously via CallbackHandler.
  auto streaming_callback = CreateCallbackHandler(request);
  int output_tokens = 0;
  StopStringFilter stop_filter(effective_options.stop_sequences);
  auto* active_stop_filter = effective_options.stop_sequences.empty() ? nullptr : &stop_filter;
  bool stop_sequence_matched = false;
  bool host_output_limit_reached = false;
  std::vector<GeneratedOutputEvent> generated_events;
  bool semantic_output_seen = false;
  bool malformed_tool_output_seen = false;

  // The template opens a new assistant turn, so only calls generated in this reply close its visible text.
  AssistantTurnGuard turn_guard;

  // Marker IDs are derived from the configured strings with the model tokenizer. This detects special markers even
  // when their decoded chunks are empty, while non-reasoning models retain the DEFAULT passthrough. The splitter is
  // seeded from the prompt so a template that already opened a reasoning block does not have its scratchpad
  // reported as visible text.
  auto splitter = CreateReasoningSplitter(cached_tool_ctx_, Model(), cached_generator_->PromptOpensReasoning());

  // Accumulator: separates visible text from tool-call blocks in the DEFAULT-segment stream. For models without
  // tool-call markers configured, both marker strings are empty and the accumulator degrades to passthrough.
  // REASONING segments bypass the accumulator entirely — tool-call-shaped text inside <think>...</think> is the
  // model's scratchpad and is not a real tool call.
  ToolCallStreamAccumulator tool_accumulator(
      cached_tool_ctx_.tool_output ? cached_tool_ctx_.tool_call_start : std::string{},
      cached_tool_ctx_.tool_output ? cached_tool_ctx_.tool_call_end : std::string{},
      cached_tool_ctx_.tools_json,
      cached_tool_ctx_.reasoning_end,
      chat_session_internal::CreateToolCallPayloadParser(cached_tool_ctx_, Model()));
  std::optional<RawEnvelopeDetector> raw_detector;
  if (const auto* descriptor = cached_tool_ctx_.ActiveRawEnvelope()) {
    raw_detector.emplace(*descriptor);
  }
  auto* active_raw_detector = raw_detector.has_value() ? &*raw_detector : nullptr;

  auto push_tool_output = [&](const std::string& text) {
    return chat_session_internal::PushToolOutput(
        text, active_raw_detector, tool_accumulator);
  };

  auto emit_tool_output = [&](ToolCallStreamAccumulator::Output out) {
    // The accumulator can emit several calls from one decoded fragment. Admit the batch atomically:
    // no caller-visible item or durable turn state may contain only its valid prefix.
    chat_session_internal::NormalizeToolOutputBatch(out, cached_tool_ctx_);
    semantic_output_seen |= HasSemanticOutput(out);
    malformed_tool_output_seen |= out.malformed;

    for (auto& event : out.events) {
      if (auto* text = std::get_if<std::string>(&event)) {
        // A structured tool call is the end of the turn's visible text: a chat template renders an assistant turn as
        // content followed by its tool calls, so text produced after a call could only be replayed as though it came
        // before one. Ending the turn here means the caller is never shown output the conversation cannot keep.
        if (!AcceptVisibleText(turn_guard, *text, logger_)) {
          continue;
        }

        AppendGeneratedSegment(generated_events, *text, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
        if (streaming_callback) {
          streaming_callback->PushItem(
              std::make_unique<TextItem>(*text, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT));
        }
        continue;
      }

      auto call = std::move(std::get<ParsedToolCall>(event));
      turn_guard.RecordToolCall();

      if (streaming_callback) {
        streaming_callback->PushItem(std::make_unique<ToolCallItem>(
            call.id, call.name, call.arguments, /*replayed_from_store=*/false,
            cached_tool_ctx_.KindOf(call.name), std::nullopt,
            call.raw_envelope ? GeneratedCallEncoding::kRawEnvelope : GeneratedCallEncoding::kStructured));
      }
      generated_events.push_back(std::move(call));
    }
  };

  auto emit_segments = [&](const std::vector<ReasoningStreamSplitter::Segment>& segments) {
    for (const auto& seg : segments) {
      if (seg.type != FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT) {
        if (active_raw_detector != nullptr && active_raw_detector->HasPendingCandidate()) {
          emit_tool_output(chat_session_internal::AbortRawToolOutput(active_raw_detector));
        }

        if (tool_accumulator.HasPayloadParser()) {
          emit_tool_output(tool_accumulator.RejectPendingSelectedPayload());
        } else if (!tool_accumulator.InsideToolCall()) {
          emit_tool_output(tool_accumulator.Flush());
        }

        // REASONING goes straight through — never feed it to the tool-call accumulator.
        semantic_output_seen |= !seg.text.empty();
        AppendGeneratedSegment(generated_events, seg.text, seg.type);
        if (streaming_callback) {
          streaming_callback->PushItem(std::make_unique<TextItem>(seg.text, seg.type));
        }
        continue;
      }

      emit_tool_output(push_tool_output(seg.text));
    }
  };

  auto flush_accumulator = [&](bool natural_end) {
    emit_tool_output(chat_session_internal::FlushToolOutput(
        active_raw_detector, tool_accumulator, natural_end));
  };

  while (!cached_generator_->IsDone() && !request.IsCancellationRequested() && !turn_guard.TurnEnded()) {
    cached_generator_->GenerateNextToken();
    const auto token_id = cached_generator_->CurrentTokenId();
    std::string token = cached_generator_->Decode();
    ++output_tokens;

    if (chat_session_internal::PushDecodedFragment(
            token, token_id, active_stop_filter, splitter, emit_segments)) {
      stop_sequence_matched = true;
      break;
    }

    if (host_max_output.has_value() &&
        chat_session_internal::DidHostOutputLimitTruncate(
            output_tokens, *host_max_output, cached_generator_->IsDone())) {
      host_output_limit_reached = true;
      break;
    }
  }

  // End-of-stream: drain the reasoning splitter first so any final DEFAULT bytes feed into the tool accumulator,
  // then drain the tool accumulator.
  chat_session_internal::FlushDecodedStream(active_stop_filter, splitter, emit_segments);

  // Callback delivery is asynchronous. Drain it before the final cancellation check and commit decision so a callback
  // that rejects the last queued item cannot arrive after this turn has already changed the transcript.
  if (streaming_callback) {
    streaming_callback->DrainPending();
  }

  // Engine generation runs asynchronously, and GetTurnUsage waits for it to finish. Stop an abandoned turn before
  // waiting, including when post-call text ended the reply without canceling the request itself.
  const bool canceled_after_drain = request.IsCancellationRequested();
  if (canceled_after_drain) {
    cached_generator_->Cancel();
  } else if ((stop_sequence_matched || host_output_limit_reached || turn_guard.TurnEnded()) &&
             !cached_generator_->IsDone()) {
    cached_generator_->Cancel();
  }

  int total_tokens = cached_generator_->TokenCount();
  int cached_prompt_tokens = 0;
  std::optional<flFinishReason> backend_finish_reason;
  std::optional<BackendTerminationCause> backend_termination;
  if (const auto turn_usage = cached_generator_->GetTurnUsage()) {
    prompt_tokens = turn_usage->prompt_tokens;
    total_tokens = turn_usage->prompt_tokens + turn_usage->generated_tokens;
    cached_prompt_tokens = turn_usage->cached_prompt_tokens;
    backend_finish_reason = turn_usage->finish_reason;
    backend_termination = turn_usage->termination_cause;
  }

  const bool natural_tool_output_end =
      !turn_guard.TurnEnded() && chat_session_internal::IsNaturalToolOutputEnd(
                                     request.IsCancellationRequested(), stop_sequence_matched,
                                     host_output_limit_reached, backend_termination);
  flush_accumulator(natural_tool_output_end);
  int accepted_reasoning_tokens = splitter.ReasoningTokenCount();

  if (streaming_callback) {
    streaming_callback->DrainPending();
  }

  if (request.IsCancellationRequested()) {
    if (!canceled_after_drain && cached_generator_) {
      cached_generator_->Cancel();
    }
    return;
  }

  bool recovered_tool_output = false;
  ToolCallContext completed_tool_ctx;
  if (malformed_tool_output_seen) {
    const bool recovery_eligible = IsGuidedEngineToolRetryEligible(
        backend_kind, natural_tool_output_end, semantic_output_seen,
        effective_options, cached_tool_ctx_);
    if (!recovery_eligible) {
      if (streaming_callback) {
        streaming_callback->Drain();
      }

      ThrowMalformedToolCallRecoveryFailure();
    }

    completed_tool_ctx = cached_tool_ctx_;
    auto retry_tool_ctx = cached_tool_ctx_;
    cached_generator_->Close();
    InvalidateCachedGenerator();

    GuidedEngineRetryResult retry;
    try {
      retry = RunGuidedEngineToolRetry(
          prepared_messages, effective_options, Model(), std::move(retry_tool_ctx), request,
          /*use_full_context=*/true, text_generator_factory_);
    } catch (...) {
      if (streaming_callback) {
        streaming_callback->Drain();
      }

      ThrowMalformedToolCallRecoveryFailure();
    }

    generated_events.clear();
    if (!retry.canceled) {
      for (auto& event : retry.events) {
        if (auto* segment = std::get_if<TextSegment>(&event)) {
          if (streaming_callback) {
            streaming_callback->PushItem(std::make_unique<TextItem>(segment->text, segment->type));
          }
        } else {
          auto& call = std::get<ParsedToolCall>(event);
          turn_guard.RecordToolCall();
          if (streaming_callback) {
            streaming_callback->PushItem(std::make_unique<ToolCallItem>(
                call.id, call.name, call.arguments, /*replayed_from_store=*/false,
                completed_tool_ctx.KindOf(call.name), std::nullopt, GeneratedCallEncoding::kStructured));
          }
        }

        generated_events.push_back(std::move(event));
      }
    }

    prompt_tokens = retry.prompt_tokens;
    total_tokens = retry.total_tokens;
    cached_prompt_tokens = retry.cached_prompt_tokens;
    backend_finish_reason = retry.finish_reason;
    accepted_reasoning_tokens = retry.reasoning_tokens;
    stop_sequence_matched = false;
    host_output_limit_reached = false;
    recovered_tool_output = true;
  }

  if (cached_generator_ && (stop_sequence_matched || host_output_limit_reached) &&
      !cached_generator_->IsDone()) {
    cached_generator_->Cancel();
  }

  if (streaming_callback) {
    streaming_callback->DrainPending();
  }

  if (request.IsCancellationRequested()) {
    if (cached_generator_) {
      cached_generator_->Cancel();
    }
    return;
  }

  const auto& output_tool_ctx = recovered_tool_output ? completed_tool_ctx : cached_tool_ctx_;
  auto assistant_message = MakeAssistantMessage(generated_events, output_tool_ctx, logger_);
  const bool generated_tool_calls = assistant_message.HasToolCalls();

  // Reject a generation whose calls cannot be correlated before it reaches the caller — a committed turn must never
  // leave the outstanding-call set inconsistent.
  transcript_.ValidateGeneratedOutput(assistant_message);

  ProcessGeneratedOutput(std::move(generated_events), output_tool_ctx, effective_options,
                         stop_sequence_matched, host_output_limit_reached, response, prompt_tokens, total_tokens,
                         accepted_reasoning_tokens, cached_prompt_tokens, backend_finish_reason);

  // LARK grammar (tool-call-only mode) is a single-shot finite parse. If generation was truncated while grammar was
  // active, the parser is in an unrecoverable state. Additionally, a completed grammar signals EOS — IsDone() would
  // return true on the next turn. Invalidate after any grammar-guided generation so the next turn rebuilds.
  //
  // Reasoning models (qwen3, etc.) also need invalidation: continuous decoding leaves prior <think> tokens in the KV
  // cache and the model fails to close subsequent reasoning blocks. The template projection does not replay reasoning
  // from plain text turns, so a rebuild restores correct behavior. This matches C#, which always applies the full
  // template per turn.
  //
  // Media is single-shot too: AppendMessages cannot extend a media-decoded sequence, so any text follow-up must
  // rebuild rather than silently feed text into a state that includes media-derived tokens.
  //
  // A turn stopped at a tool call because the model kept talking afterwards is truncated mid-sequence with no turn
  // terminator, so its KV cache is no basis for the next turn either.
  const bool grammar_was_active =
      recovered_tool_output ||
      ResolveTurnGuidanceOptions(cached_tool_ctx_, cached_generator_->PromptOpensReasoning()).has_value();
  const bool reasoning_was_active = output_tool_ctx.supports_reasoning;
  const bool discard_after_success =
      recovered_tool_output ||
      chat_session_internal::ShouldInvalidateRetainedGenerationStateAfterSuccessfulTurn(
          backend_kind, grammar_was_active, reasoning_was_active, stop_sequence_matched,
          host_output_limit_reached);

  // Claim successful completion before committing the turn so cancellation cannot win after the transcript changes.
  if (!request.TryComplete()) {
    return;
  }

  transcript_.CommitTurn(std::move(inputs), std::move(assistant_message),
                         {pre_turn_token_count, total_tokens});
  turn_committed = true;

  if (discard_after_success || media_turn || generated_tool_calls || turn_guard.TurnEnded()) {
    InvalidateCachedGenerator();
  }
}

void ChatSession::ProcessChatCompletionsJson(PreparedChatRequest& prepared, const Request& original_request,
                                             Response& response) {
  const auto& model_name = prepared.json_model_name;
  std::string completion_id = chat_completions::GenerateCompletionId();
  auto now = std::chrono::system_clock::now();
  int64_t created = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

  const auto& tool_ctx = prepared.tool_context;
  const auto& options = prepared.options;
  const auto& prepared_messages = *prepared.messages;
  auto generator =
      CreatePreparedTextChatGenerator(prepared_messages, std::move(prepared.prompt), options, Model(), tool_ctx,
                                      /*use_full_context=*/false, text_generator_factory_);
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
                                             tool_ctx.tool_output ? tool_ctx.tool_call_end : std::string{},
                                             tool_ctx.tools_json,
                                             tool_ctx.reasoning_end,
                                             chat_session_internal::CreateToolCallPayloadParser(
                                                 tool_ctx, Model()));
  std::optional<RawEnvelopeDetector> raw_detector;
  if (const auto* descriptor = tool_ctx.ActiveRawEnvelope()) {
    raw_detector.emplace(*descriptor);
  }
  auto* active_raw_detector = raw_detector.has_value() ? &*raw_detector : nullptr;

  auto push_tool_output = [&](const std::string& text) {
    return chat_session_internal::PushToolOutput(
        text, active_raw_detector, tool_accumulator);
  };

  int next_tool_call_index = 0;
  std::vector<GeneratedOutputEvent> generated_events;
  bool semantic_output_seen = false;
  bool malformed_tool_output_seen = false;

  AssistantTurnGuard turn_guard;

  // Use the same typed segments for streaming and final response construction.
  auto splitter = CreateReasoningSplitter(tool_ctx, Model(), generator->PromptOpensReasoning());

  auto emit_visible_text = [&](std::string visible) {
    if (visible.empty() || !is_streaming) {
      return;
    }

    auto chunk_json = chat_completions::FormatStreamingChunk(visible, completion_id, created, model_name);
    streaming_callback->PushItem(std::make_unique<TextItem>(std::move(chunk_json),
                                                            FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
  };

  auto process_tool_output = [&](ToolCallStreamAccumulator::Output out) {
    // Validate and normalize the entire parsed batch before publishing any element. In particular,
    // custom input comes from argument_source, which preserves the complete provider wrapper.
    chat_session_internal::NormalizeToolOutputBatch(out, tool_ctx);
    semantic_output_seen |= HasSemanticOutput(out);
    malformed_tool_output_seen |= out.malformed;

    for (auto& event : out.events) {
      if (auto* text = std::get_if<std::string>(&event)) {
        // A chat completion carries the assistant reply as `content` plus a `tool_calls` array — the same schema the
        // transcript projection uses, with the same limitation. Text produced after a call would be reported in
        // `content`, in front of the call that actually came first, and a client replaying that message back would
        // be replaying an order of events that never happened. End the turn at the call instead.
        if (!AcceptVisibleText(turn_guard, *text, logger_)) {
          continue;
        }

        AppendGeneratedSegment(generated_events, *text, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
        emit_visible_text(*text);
        continue;
      }

      auto call = std::move(std::get<ParsedToolCall>(event));
      turn_guard.RecordToolCall();

      if (is_streaming) {
        auto streamed = chat_completions::MakeToolCall(call.id, call.name, call.arguments,
                                                       tool_ctx.KindOf(call.name));
        streamed.index = next_tool_call_index++;
        auto chunk_json = chat_completions::FormatToolCallStreamingChunk(
            {streamed}, completion_id, created, model_name);
        streaming_callback->PushItem(std::make_unique<TextItem>(
            std::move(chunk_json), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
      }
      generated_events.push_back(std::move(call));
    }
  };

  auto process_segments = [&](const std::vector<ReasoningStreamSplitter::Segment>& segments) {
    for (const auto& seg : segments) {
      // REASONING segments: never feed reasoning text to the tool-call accumulator — tool-call-shaped text inside
      // <think>...</think> is scratchpad, not a real call. Emit via reasoning_content, not content.
      if (seg.type != FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT) {
        if (active_raw_detector != nullptr && active_raw_detector->HasPendingCandidate()) {
          process_tool_output(chat_session_internal::AbortRawToolOutput(active_raw_detector));
        }

        if (tool_accumulator.HasPayloadParser()) {
          process_tool_output(tool_accumulator.RejectPendingSelectedPayload());
        } else if (!tool_accumulator.InsideToolCall()) {
          process_tool_output(tool_accumulator.Flush());
        }

        semantic_output_seen |= !seg.text.empty();
        AppendGeneratedSegment(generated_events, seg.text, seg.type);

        if (is_streaming && !seg.text.empty()) {
          auto chunk_json = chat_completions::FormatReasoningStreamingChunk(
              seg.text, completion_id, created, model_name);
          streaming_callback->PushItem(std::make_unique<TextItem>(
              std::move(chunk_json), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
        }
        continue;
      }

      process_tool_output(push_tool_output(seg.text));
    }
  };

  // Generate token-by-token.
  StopStringFilter stop_filter(options.stop_sequences);
  auto* active_stop_filter = options.stop_sequences.empty() ? nullptr : &stop_filter;
  bool stop_sequence_matched = false;
  while (!generator->IsDone() && !original_request.IsCancellationRequested() && !turn_guard.TurnEnded()) {
    generator->GenerateNextToken();
    const auto token_id = generator->CurrentTokenId();
    std::string token = generator->Decode();

    if (chat_session_internal::PushDecodedFragment(
            token, token_id, active_stop_filter, splitter, process_segments)) {
      stop_sequence_matched = true;
      break;
    }
  }

  // Drain any buffered partial-marker bytes at end-of-stream. Reasoning splitter first so any final DEFAULT bytes
  // feed into the tool accumulator; then drain the tool accumulator.
  chat_session_internal::FlushDecodedStream(active_stop_filter, splitter, process_segments);

  // Delivery is asynchronous. Observe cancellation from the last content callback before deriving a success finish
  // reason, publishing a terminal chunk, or constructing the final completion envelope.
  if (streaming_callback) {
    streaming_callback->DrainPending();
  }

  // Cancel host-ended turns before waiting for asynchronous Engine usage.
  const bool canceled_after_drain = original_request.IsCancellationRequested();
  if (canceled_after_drain) {
    generator->Cancel();
  } else if ((stop_sequence_matched || turn_guard.TurnEnded()) && !generator->IsDone()) {
    generator->Cancel();
  }

  int total_tokens = generator->TokenCount();
  int cached_prompt_tokens = 0;
  std::optional<flFinishReason> backend_finish_reason;
  std::optional<BackendTerminationCause> backend_termination;
  if (const auto turn_usage = generator->GetTurnUsage()) {
    prompt_tokens = turn_usage->prompt_tokens;
    total_tokens = turn_usage->prompt_tokens + turn_usage->generated_tokens;
    cached_prompt_tokens = turn_usage->cached_prompt_tokens;
    backend_finish_reason = turn_usage->finish_reason;
    backend_termination = turn_usage->termination_cause;
  }

  const bool natural_tool_output_end =
      !turn_guard.TurnEnded() && chat_session_internal::IsNaturalToolOutputEnd(
                                     original_request.IsCancellationRequested(), stop_sequence_matched,
                                     /*host_output_limit_reached=*/false, backend_termination);
  process_tool_output(chat_session_internal::FlushToolOutput(
      active_raw_detector, tool_accumulator, natural_tool_output_end));
  int accepted_reasoning_tokens = splitter.ReasoningTokenCount();

  if (streaming_callback) {
    streaming_callback->DrainPending();
  }

  if (original_request.IsCancellationRequested()) {
    if (!canceled_after_drain) {
      generator->Cancel();
    }
    return;
  }

  if (malformed_tool_output_seen) {
    const bool recovery_eligible = IsGuidedEngineToolRetryEligible(
        Model().GetGenAIConfig().GetChatBackendKind(), natural_tool_output_end,
        semantic_output_seen, options, tool_ctx);
    if (!recovery_eligible) {
      if (streaming_callback) {
        streaming_callback->Drain();
      }

      ThrowMalformedToolCallRecoveryFailure();
    }

    generator->Close();
    generator.reset();
    GuidedEngineRetryResult retry;
    try {
      retry = RunGuidedEngineToolRetry(
          prepared_messages, options, Model(), tool_ctx, original_request,
          /*use_full_context=*/false, text_generator_factory_);
    } catch (...) {
      if (streaming_callback) {
        streaming_callback->Drain();
      }

      ThrowMalformedToolCallRecoveryFailure();
    }

    generated_events.clear();
    if (!retry.canceled) {
      for (auto& event : retry.events) {
        if (auto* segment = std::get_if<TextSegment>(&event)) {
          AppendGeneratedSegment(generated_events, segment->text, segment->type);
          if (is_streaming && !segment->text.empty()) {
            auto chunk_json = chat_completions::FormatReasoningStreamingChunk(
                segment->text, completion_id, created, model_name);
            streaming_callback->PushItem(std::make_unique<TextItem>(
                std::move(chunk_json), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
          }
        } else {
          ToolCallStreamAccumulator::Output output;
          output.events.emplace_back(std::move(std::get<ParsedToolCall>(event)));
          process_tool_output(std::move(output));
        }
      }
    }

    prompt_tokens = retry.prompt_tokens;
    total_tokens = retry.total_tokens;
    cached_prompt_tokens = retry.cached_prompt_tokens;
    backend_finish_reason = retry.finish_reason;
    accepted_reasoning_tokens = retry.reasoning_tokens;
    stop_sequence_matched = false;
  }

  if (generator && stop_sequence_matched && !generator->IsDone()) {
    generator->Cancel();
  }

  if (streaming_callback) {
    streaming_callback->DrainPending();
  }

  if (original_request.IsCancellationRequested()) {
    if (generator) {
      generator->Cancel();
    }
    return;
  }

  ProcessGeneratedOutput(std::move(generated_events), tool_ctx, options,
                         stop_sequence_matched, /*host_output_limit_reached=*/false, response, prompt_tokens,
                         total_tokens, accepted_reasoning_tokens, cached_prompt_tokens, backend_finish_reason);

  if (!original_request.TryComplete()) {
    return;
  }

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

const ChatTranscript& ChatSession::Transcript() const {
  return transcript_;
}

void ChatSession::InvalidateCachedGenerator() noexcept {
  cached_generator_.reset();
  cached_tool_ctx_ = {};
  // The prefix describes the dropped generator's prompt; the next turn supplies its own.
  system_prompt_.clear();
}

size_t ChatSession::TurnCount() const {
  return transcript_.TurnCount();
}

void ChatSession::UndoTurns(size_t count) {
  auto request_lock = LockRequestMutex();

  if (count == 0) {
    return;
  }

  const bool undo_all = count == transcript_.TurnCount();

  // The transcript validates `count` and rolls back messages, turn records, and outstanding-call state as one step.
  auto tokens = transcript_.UndoTurns(count);

  if (!cached_generator_) {
    return;
  }

  if (chat_session_internal::ShouldInvalidateRetainedGeneratorForUndo(
          undo_all, tokens.pre_turn.has_value(), cached_generator_->CanRewind())) {
    // Undoing every turn, or undoing back to a turn whose generator was rebuilt — in both cases the current KV cache
    // has no usable boundary matching the target state. A non-rewindable backend must likewise rebuild from the
    // already-truncated transcript.
    InvalidateCachedGenerator();
    return;
  }

  // The transcript is already truncated. A failed rewind would leave the KV cache describing a conversation the
  // transcript no longer has, so drop the generator before the failure propagates.
  ScopeGuard invalidate_on_failed_rewind([this]() noexcept { InvalidateCachedGenerator(); });
  cached_generator_->RewindTo(*tokens.pre_turn);
  invalidate_on_failed_rewind.Dismiss();
}

size_t ChatSession::MessageCount() const {
  return transcript_.MessageCount();
}

}  // namespace fl
