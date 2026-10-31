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
#include "inferencing/generative/toolcalling/assistant_text_framer.h"
#include "inferencing/generative/toolcalling/grammar.h"
#include "inferencing/generative/toolcalling/json_tool_call_payload_parser.h"
#include "inferencing/generative/toolcalling/tool_call_recovery_checkpoint.h"
#include "inferencing/generative/toolcalling/raw_envelope_detector.h"
#include "inferencing/generative/toolcalling/tool_call_stream_accumulator.h"
#include "inferencing/generative/toolcalling/tool_call_utils.h"
#include "inferencing/session/tool_registry.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "model.h"
#include "telemetry/telemetry_request_metrics.h"
#include "util/scope_guard.h"
#include "util/json_helpers.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <ort_genai.h>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>

namespace fl {

namespace {

// Translate a parsed tool_choice into the text_output / tool_output flags on the tool-call context.
// Defaults (nullopt) match "auto": the model is free to emit text or tool calls.
void ApplyToolChoiceToContext(std::optional<flToolChoice> tool_choice, ToolCallContext& tool_ctx) {
  if (tool_ctx.forced_tool) {
    if (tool_choice == FOUNDRY_LOCAL_TOOL_CHOICE_NONE) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "a forced tool conflicts with tool_choice=none");
    }
    tool_ctx.text_output = false;
    tool_ctx.tool_output = true;
    return;
  }
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

// Framed turns match stops only after protocol removal and tool/reasoning classification. Validate the original
// visible bytes before filtering so a stop at the first visible byte is not mistaken for an empty model frame.
class AssistantTextOutputFilter {
 public:
  AssistantTextOutputFilter(const ToolCallContext& context, const SearchOptions& options)
      : framer_(context),
        stop_filter_(context.text_output && context.tool_output
                         ? options.stop_sequences
                         : std::vector<std::string>{}),
        text_output_(context.text_output) {}

  std::string Push(std::string_view text) {
    auto visible = stop_filter_.Push(framer_.Push(text));
    if (!text_output_) {
      return {};
    }
    return visible;
  }

  std::string Flush() {
    auto visible = stop_filter_.Push(framer_.Flush());
    visible += stop_filter_.Flush();
    if (!text_output_) {
      return {};
    }
    return visible;
  }

  bool Matched() const {
    return stop_filter_.matched();
  }

  void ValidateBoundary() const {
    framer_.ValidateBoundary();
  }

  void ValidateCompletion(bool has_tool_calls) const {
    framer_.ValidateCompletion(has_tool_calls);
  }

 private:
  AssistantTextFramer framer_;
  StopStringFilter stop_filter_;
  bool text_output_;
};

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
  ReasoningStreamSplitter splitter{
      std::move(markers.start), std::move(markers.end), std::move(markers.start_token_ids),
      std::move(markers.end_token_ids), std::move(ignored_token_ids),
      chat_session_internal::ShouldStartInsideReasoning(tool_ctx, prompt_opens_reasoning)};
  if (tool_ctx.tool_plan && tool_ctx.reasoning_start_token_id && tool_ctx.reasoning_end_token_id) {
    splitter.UseTokenBoundariesOnly();
  }

  return splitter;
}

std::string RestoreToolBoundary(std::string decoded, std::optional<int32_t> token_id,
                                const ToolCallContext& context, bool inside_reasoning, bool inside_tool) {
  if (context.tool_plan && decoded.empty() && !inside_reasoning) {
    if (token_id && token_id == context.tool_call_start_token_id) {
      return context.tool_call_start;
    }
    if (inside_tool && token_id && token_id == context.tool_call_end_token_id) {
      return context.tool_call_end;
    }
    if (inside_tool && token_id && token_id == context.reasoning_start_token_id) {
      return context.reasoning_start;
    }
    if (inside_tool && token_id && token_id == context.reasoning_end_token_id) {
      return context.reasoning_end;
    }
  }

  return decoded;
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

bool HasGeneratedToolCalls(const std::vector<GeneratedOutputEvent>& events) {
  return std::ranges::any_of(events, [](const auto& event) { return std::holds_alternative<ParsedToolCall>(event); });
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
  nlohmann::ordered_json tools_array = nlohmann::ordered_json::array();
  context.tool_kinds = tools::KindsByName(definitions);

  for (const auto& definition : definitions) {
    if (definition.custom_lark_grammar.has_value()) {
      context.custom_lark_grammars.emplace(definition.name, *definition.custom_lark_grammar);
    }

    if (definition.name.empty()) {
      const auto serialized = ParseJsonStrict(definition.json_schema);
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

    nlohmann::ordered_json tool;
    tool["type"] = "function";
    tool["function"]["name"] = definition.name;

    if (definition.include_description_in_prompt) {
      tool["function"]["description"] = definition.description;
    }

    if (definition.include_parameters_in_prompt && !definition.json_schema.empty()) {
      tool["function"]["parameters"] = ParseJsonStrict(definition.json_schema);
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
  if (raw == nullptr) {
    return;
  }
  if (!context.forced_tool) {
    if (!context.HasAnyExplicitGuidance()) {
      context.guidance_disabled = true;
      logger.Log(LogLevel::Debug, "Automatic raw-envelope output uses descriptor framing, not canonical JSON guidance");
    }
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

ToolCallPayloadParser CreateToolCallPayloadParser(const ToolCallContext& context) {
  if (context.tool_plan && context.tool_output && context.HasToolCallTokens()) {
    return CreateJsonToolCallPayloadParser(context.tool_plan, context.GeneratedToolCallStart(),
                                           context.GeneratedToolCallEnd(),
                                           context.parallel_tool_calls,
                                           context.fenced_tool_call_output,
                                           context.fenced_tool_call_output && context.text_output);
  }

  return {};
}

void NormalizeToolOutputBatch(ToolCallStreamAccumulator::Output& output,
                              const ToolCallContext& tool_ctx) {
  if (tool_ctx.tool_plan) {
    for (const auto& event : output.events) {
      if (const auto* call = std::get_if<ParsedToolCall>(&event); call != nullptr && !call->raw_envelope) {
        tool_ctx.tool_plan->ValidateCall(*call);
      }
    }
  }

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
  if (destination.malformed) {
    return;
  }

  destination.events.insert(destination.events.end(),
                            std::make_move_iterator(source.events.begin()),
                            std::make_move_iterator(source.events.end()));
  destination.malformed = source.malformed;
  if (source.malformed) {
    destination.error = std::move(source.error);
  }
}

bool ContainsToolCall(const ToolCallStreamAccumulator::Output& output) {
  return std::ranges::any_of(output.events, [](const auto& event) {
    return std::holds_alternative<ParsedToolCall>(event);
  });
}

ToolCallStreamAccumulator::Output RouteRawToolOutput(
    RawEnvelopeDetector::Output raw_output,
    RawEnvelopeDetector& raw_detector,
    ToolCallStreamAccumulator& structured_accumulator,
    bool recognize_structured = true) {
  ToolCallStreamAccumulator::Output output;
  bool structured_call_completed = false;
  const bool output_contains_raw_call =
      std::ranges::any_of(raw_output.events, [](const auto& event) {
        return std::holds_alternative<ParsedToolCall>(event);
      });
  bool raw_call_completed =
      raw_detector.CallCompleted() && !output_contains_raw_call;

  const auto push_structured = [&](const std::string& text) {
    if (!recognize_structured) {
      output.events.emplace_back(text);
      return;
    }
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
    if (output.malformed) {
      break;
    }

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
      if (!output.malformed) {
        output.events.emplace_back(std::move(rejected->text));
      }
      continue;
    }

    // Raw envelopes interrupt the structured byte stream. Release any structured prefix first
    // so caller-visible event ordering remains identical to model output ordering.
    auto structured_output = structured_accumulator.Flush();
    structured_call_completed |= ContainsToolCall(structured_output);
    AppendToolOutput(output, std::move(structured_output));

    auto raw_call = std::move(std::get<ParsedToolCall>(event));
    if (output.malformed) {
      break;
    }
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
    if (output.malformed) {
      break;
    }

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

ToolCallStreamAccumulator::Output PushToolOutputSegment(
    const ReasoningStreamSplitter::Segment& segment, const ToolCallContext& context,
    RawEnvelopeDetector* raw_detector, ToolCallStreamAccumulator& structured_accumulator) {
  if (!context.fenced_tool_call_output && context.tool_plan &&
      context.tool_call_start_token_id && context.tool_call_end_token_id &&
      (raw_detector == nullptr || !raw_detector->InsideEnvelope() || structured_accumulator.InsideToolCall())) {
    auto ownership = structured_accumulator.ObserveTokenBoundaries(
        segment.text, segment.token_id == context.tool_call_start_token_id,
        segment.token_id == context.tool_call_end_token_id);
    if (ownership.malformed) {
      return ownership;
    }
  }

  if (!context.fenced_tool_call_output && context.tool_plan &&
      context.tool_call_start_token_id && !structured_accumulator.InsideToolCall() &&
      segment.token_id != context.tool_call_start_token_id) {
    if (raw_detector != nullptr) {
      return RouteRawToolOutput(raw_detector->Push(segment.text), *raw_detector, structured_accumulator,
                                /*recognize_structured=*/false);
    }
    ToolCallStreamAccumulator::Output output;
    output.events.emplace_back(segment.text);
    return output;
  }

  return PushToolOutput(segment.text, raw_detector, structured_accumulator);
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
  if (output.malformed) {
    return output;
  }

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

bool ShouldRebuildRetainedGeneratorBeforeAppend(bool guidance_requirement_changed, bool guidance_payload_changed,
                                                bool retained_generation_settings_changed) {
  return guidance_requirement_changed || guidance_payload_changed || retained_generation_settings_changed;
}

bool ShouldInvalidateRetainedGenerationStateAfterSuccessfulTurn(const ToolCallContext& tool_ctx,
                                                                bool prompt_opens_reasoning, bool recovered_tool_output,
                                                                bool reasoning_was_active, bool stop_sequence_matched,
                                                                bool host_output_limit_reached) {
  return stop_sequence_matched || host_output_limit_reached || reasoning_was_active ||
         recovered_tool_output || ResolveTurnGuidanceOptions(tool_ctx, prompt_opens_reasoning).has_value();
}

bool ShouldInvalidateRetainedGeneratorForUndo(bool undo_all, bool has_pre_turn_boundary, bool can_rewind) {
  return undo_all || !has_pre_turn_boundary || !can_rewind;
}

}  // namespace chat_session_internal

namespace {

struct GuidedToolRetryResult {
  std::vector<GeneratedOutputEvent> events;
  int prompt_tokens = 0;
  int total_tokens = 0;
  int reasoning_tokens = 0;
  int cached_prompt_tokens = 0;
  std::optional<flFinishReason> finish_reason;
  std::optional<BackendTerminationCause> termination_cause;
  bool canceled = false;
};

[[noreturn]] void ThrowInvalidGuidedToolRetry(const std::string& detail) {
  FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
           "Model emitted a malformed tool call and guided recovery did not produce a valid tool call: " + detail);
}

std::vector<ParsedToolCall> ParseStrictGuidedToolCalls(const std::string& text, const ToolCallContext& tool_ctx) {
  const auto first = text.find_first_not_of(" \t\r\n");
  const auto last = text.find_last_not_of(" \t\r\n");
  if (first == std::string::npos || last == std::string::npos) {
    return {};
  }

  const auto trimmed = std::string_view(text).substr(first, last - first + 1);
  if (trimmed.size() < tool_ctx.tool_call_start.size() + tool_ctx.tool_call_end.size() ||
      !trimmed.starts_with(tool_ctx.tool_call_start) || !trimmed.ends_with(tool_ctx.tool_call_end)) {
    return {};
  }

  const auto payload = trimmed.substr(tool_ctx.tool_call_start.size(),
                                      trimmed.size() - tool_ctx.tool_call_start.size() - tool_ctx.tool_call_end.size());
  return tool_ctx.tool_plan->ParseCalls(payload);
}

GuidedToolRetryResult RunGuidedToolRetry(const chat_internal::PreparedChatMessages& messages,
                                         const SearchOptions& options, GenAIModelInstance& model,
                                         ToolCallContext tool_ctx, const Request& request,
                                         const TextChatGeneratorFactory& factory, const std::string& validation_error,
                                         std::optional<PreparedChatPrompt> retry_prompt = std::nullopt) {
  if (!tool_ctx.tool_plan || !options.stop_sequences.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Guided tool recovery requires a schema plan and no stop sequences");
  }

  tool_ctx.ConfigureForGuidedToolRetry();
  if (retry_prompt) {
    tool_ctx.supports_reasoning = false;
  }

  if (!retry_prompt) {
    retry_prompt = PrepareTextChatPrompt(messages, model, tool_ctx);
  }

  // Keep the published prefix's token identity. Encode private correction data literally, never as controls.
  auto diagnostic = nlohmann::json(validation_error).dump(-1, ' ', true);
  if (diagnostic.size() > 4096) {
    diagnostic.resize(4096);
    diagnostic += " [diagnostic truncated]";
  }
  const auto correction =
      " The previous unexecuted tool call was rejected: " + diagnostic +
      ". Emit a corrected tool call satisfying the original tool schema and the user's requested values. "
      "Do not repeat earlier reasoning or text. ";
  const auto sequences = model.GetPreprocessor().EncodeLiteral(correction.c_str());
  const auto* data = sequences->SequenceData(0);
  retry_prompt->token_ids.insert(retry_prompt->token_ids.end(), data, data + sequences->SequenceCount(0));
  retry_prompt->prompt += correction;
  retry_prompt->prompt_token_count = static_cast<int64_t>(retry_prompt->token_ids.size());

  auto generator = CreatePreparedTextChatGenerator(messages, std::move(*retry_prompt), options, model, tool_ctx,
                                                   /*use_full_context=*/false, factory);
  auto splitter = CreateReasoningSplitter(tool_ctx, model, generator->PromptOpensReasoning());

  GuidedToolRetryResult result;
  std::string structured_output;
  std::string token_boundary_error;
  ToolCallStreamAccumulator boundary_validator(tool_ctx.tool_call_start, tool_ctx.tool_call_end);
  auto process_segments = [&](const std::vector<TextSegment>& segments) {
    for (const auto& segment : segments) {
      if (segment.type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT) {
        if (tool_ctx.tool_call_start_token_id && tool_ctx.tool_call_end_token_id) {
          const auto ownership = boundary_validator.ObserveTokenBoundaries(
              segment.text, segment.token_id == tool_ctx.tool_call_start_token_id,
              segment.token_id == tool_ctx.tool_call_end_token_id);
          if (ownership.malformed) {
            token_boundary_error = ownership.error;
            return;
          }
        }
        structured_output += segment.text;
      } else {
        AppendGeneratedSegment(result.events, segment.text, segment.type);
      }
    }
  };

  bool tool_payload_started = false;
  int output_tokens = 0;
  while (!generator->IsDone() && !request.IsCancellationRequested() && token_boundary_error.empty() &&
         (!options.max_output_tokens || output_tokens < *options.max_output_tokens)) {
    generator->GenerateNextToken();
    ++output_tokens;
    const auto token_id = generator->CurrentTokenId();
    auto token =
        RestoreToolBoundary(generator->Decode(), token_id, tool_ctx, splitter.InsideReasoning(), tool_payload_started);
    if (!splitter.InsideReasoning() && token_id && token_id == tool_ctx.tool_call_start_token_id) {
      tool_payload_started = true;
    }
    chat_session_internal::DeliverDecodedFragment(std::move(token), token_id, splitter, process_segments,
                                                  tool_payload_started);
  }

  result.canceled = request.IsCancellationRequested();
  const bool host_output_limit_reached =
      options.max_output_tokens &&
      chat_session_internal::DidHostOutputLimitTruncate(output_tokens, *options.max_output_tokens, generator->IsDone());
  if (result.canceled) {
    generator->Cancel();
  }

  process_segments(splitter.Flush());

  if (!result.canceled && request.IsCancellationRequested()) {
    result.canceled = true;
    generator->Cancel();
  } else if ((host_output_limit_reached || !token_boundary_error.empty()) && !generator->IsDone()) {
    generator->Cancel();
  }

  result.prompt_tokens = generator->PromptTokenCount();
  result.total_tokens = generator->TokenCount();
  if (const auto usage = generator->GetTurnUsage()) {
    result.prompt_tokens = usage->prompt_tokens;
    result.total_tokens = usage->prompt_tokens + usage->generated_tokens;
    result.cached_prompt_tokens = usage->cached_prompt_tokens;
    result.finish_reason = usage->finish_reason;
    result.termination_cause = usage->termination_cause;
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

  if (!token_boundary_error.empty()) {
    ThrowInvalidGuidedToolRetry(token_boundary_error);
  }

  const bool natural_end = chat_session_internal::IsNaturalToolOutputEnd(
      /*canceled=*/false, /*stop_sequence_matched=*/false,
      host_output_limit_reached ||
          (options.max_output_tokens && result.total_tokens - result.prompt_tokens > *options.max_output_tokens),
      result.termination_cause);
  result.reasoning_tokens = splitter.ReasoningTokenCount();

  if (tool_ctx.tool_call_start_token_id && tool_ctx.tool_call_end_token_id &&
      !boundary_validator.HasCompleteTokenOwnedEnvelope()) {
    ThrowInvalidGuidedToolRetry("Missing token-owned tool-call boundaries");
  }

  auto calls = ParseStrictGuidedToolCalls(structured_output, tool_ctx);
  if (!natural_end || calls.empty()) {
    ThrowInvalidGuidedToolRetry("Incomplete, interrupted, or over-budget tool-call output");
  }

  for (auto& call : calls) {
    ToolCallStreamAccumulator::Output output;
    output.events.emplace_back(std::move(call));
    chat_session_internal::NormalizeToolOutputBatch(output, tool_ctx);
    result.events.emplace_back(std::move(std::get<ParsedToolCall>(output.events.front())));
  }

  return result;
}

[[noreturn]] void ThrowInvalidToolCallWithoutRecovery(const std::string& detail) {
  FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
           "Model emitted an invalid tool call: " + detail +
               ". Recovery requires an uncommitted call, an exact streamed-prefix checkpoint, and remaining "
               "output budget; canceled or truncated turns are not replayed");
}

bool IsGuidedToolRetryEligible(
    bool natural_tool_output_end,
    bool semantic_output_seen,
    const SearchOptions& options,
    const ToolCallContext& tool_ctx,
    bool safe_prefix_available,
    bool tool_call_published,
    int attempted_tokens,
    std::optional<int> output_budget) {
  return tool_ctx.tool_plan && natural_tool_output_end &&
         (!semantic_output_seen || safe_prefix_available) && !tool_call_published &&
         (!output_budget || attempted_tokens < *output_budget) &&
         options.stop_sequences.empty() &&
         tool_ctx.tool_output &&
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
  return std::string(EPUtils::EPtoTelemetryName(model_.EP(), model_.GetGenAIConfig().DefaultProviderView()));
}

void ChatSession::SetSessionOptionsImpl(const KeyValuePairs& options) {
  session_options_ = SearchOptions::FromParameters(options);
}

namespace {

ToolCallContext BuildToolCallContextForRequest(const Request& request,
                                               const KeyValuePairs& effective_options,
                                               const std::vector<ToolDefinition>& definitions,
                                               const ModelInfo& model_info,
                                               GenAIModelInstance& model,
                                               ILogger& logger) {
  ToolCallContext tool_ctx;

  tool_ctx.tool_call_start = GetOptionOrEmpty(effective_options, FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_START_STR);
  tool_ctx.tool_call_end = GetOptionOrEmpty(effective_options, FOUNDRY_LOCAL_MODEL_PROP_TOOL_CALL_END_STR);
  const auto parallel_tool_calls = GetOptionOrEmpty(effective_options, "parallel_tool_calls");
  if (parallel_tool_calls == "false") {
    tool_ctx.parallel_tool_calls = false;
  } else if (!parallel_tool_calls.empty() && parallel_tool_calls != "true") {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "parallel_tool_calls must be true or false");
  }
  tool_ctx.template_kwargs_json = GetOptionOrEmpty(effective_options, "chat_template_kwargs");
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
  if (!tool_ctx.template_kwargs_json.empty()) {
    const auto kwargs = nlohmann::json::parse(tool_ctx.template_kwargs_json);
    if (const auto thinking = kwargs.find("enable_thinking");
        thinking != kwargs.end() && thinking->is_boolean() && !thinking->get<bool>()) {
      tool_ctx.supports_reasoning = false;
    }
  }

  // Read reasoning marker tokens — same pattern as tool_call tokens
  tool_ctx.reasoning_start = GetOptionOrEmpty(effective_options, FOUNDRY_LOCAL_MODEL_PROP_REASONING_START_STR);
  tool_ctx.reasoning_end = GetOptionOrEmpty(effective_options, FOUNDRY_LOCAL_MODEL_PROP_REASONING_END_STR);

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
  const auto tool_choice = SearchOptions::ParseToolChoice(effective_options);

  // Explicit response guidance stays independent of generated tool guidance; declarations still require admission.
  tool_ctx.guidance_type = GetOptionOrEmpty(effective_options, "guidance_type");
  tool_ctx.guidance_data = GetOptionOrEmpty(effective_options, "guidance_data");
  if (tool_ctx.HasPartialExplicitGuidance()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "guidance_type and guidance_data must be provided together");
  }
  if (tool_ctx.HasTools()) {
    ApplyToolChoiceToContext(tool_choice, tool_ctx);

    if (tool_ctx.tool_output) {
      if (!tool_ctx.HasToolCallTokens() && !tool_ctx.ActiveRawEnvelope()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                 "structured tool calling requires model-published opening and closing tool markers");
      }
      tool_ctx.tool_plan = ToolCallPlan::Create(tool_ctx.tools_json, tool_ctx.parallel_tool_calls,
                                                tool_ctx.forced_tool);
      tool_ctx.eos_token_ids = model.GetPreprocessor().GetEosTokenIds();
      if (!request.raw_envelope_descriptor && !tool_ctx.forced_tool) {
        tool_ctx.raw_envelope.reset();
      }
      tool_ctx.native_assistant_text_output = tool_ctx.text_output;
      if (model.ModelId().starts_with("qwen2.5-coder-0.5b") && !tool_ctx.ActiveRawEnvelope() &&
          !tool_ctx.HasAnyExplicitGuidance()) {
        tool_ctx.fenced_tool_call_output = true;
        tool_ctx.guidance_disabled = true;
      }
      tool_ctx.tool_body_grammar =
          PlanDelimitedToolBodyGuidance(tool_ctx, model.GetGenAIConfig().GetChatBackendKind());
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
  std::optional<uint64_t> json_message_count{0};
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

    auto request_json = ParseJsonStrict(text_item.text);
    if (model_info.task == "coding") {
      // Validate media before the text-only chat contract parser discards non-text content parts.
      for (const auto& message : request_json.at("messages")) {
        const auto content = message.find("content");
        if (content == message.end() || !content->is_array()) {
          continue;
        }

        for (const auto& part : *content) {
          if (!part.is_object()) {
            continue;
          }

          const auto type = part.value("type", "");
          if (type == "image_url" || type == "input_image" || type == "image") {
            FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "IMAGE input is not supported by model task 'coding'");
          }

          if (type == "input_audio" || type == "audio") {
            FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "AUDIO input is not supported by model task 'coding'");
          }
        }
      }
    }

    auto chat_request = request_json.get<ChatCompletionRequest>();
    prepared->json_message_count = TelemetryInternal::CountParsedJsonMessages(request_json);
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

    const auto effective_kvp = MergeKeyValuePairs(base_session_options, internal_request.options);
    prepared->tool_context = BuildToolCallContextForRequest(internal_request, effective_kvp, request_definitions,
                                                            model_info, model, logger);
    prepared->options = SearchOptions::FromParameters(effective_kvp);
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

  auto effective_kvp = MergeKeyValuePairs(base_session_options, request.options);
  chat_completions::ApplyCatalogDefaults(effective_kvp, model_info.model_settings);
  prepared->media = CollectMediaInput(request);
  const bool media_turn = !prepared->media.Empty();
  if (media_turn) {
    ToolCallContext declarations;
    chat_session_internal::PopulateToolDefinitions(tool_definitions, declarations);
    ValidateMediaTurn(prepared->media, BuildTranscriptMessages(request.items),
                      {.session_has_history = !transcript.Empty(), .tools_declared = declarations.HasTools()});
  }

  prepared->tool_context = BuildToolCallContextForRequest(request, effective_kvp, tool_definitions,
                                                          model_info, model, logger);
  prepared->ingest = IngestRequestItems(request.items, request.item_segment_starts,
                                        prepared->tool_context.tool_kinds);
  transcript.ValidateInputs(prepared->ingest.messages);
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
                                std::vector<ToolDefinition> tool_definitions,
                                ModelInfo model_info,
                                GenAIModelInstance& model,
                                ILogger& logger,
                                ChatMessagePreparer message_preparer)
      : request_(std::move(request)),
        transcript_(std::move(transcript)),
        base_session_options_(std::move(base_session_options)),
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
    auto prepared = PrepareChatRequest(request, transcript_, base_session_options_,
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
  std::vector<ToolDefinition> tool_definitions_;
  ModelInfo model_info_;
  GenAIModelInstance& model_;
  ILogger& logger_;
  ChatMessagePreparer message_preparer_;
};

}  // namespace

std::unique_ptr<Session::RequestPreflightOperation> ChatSession::CreateRequestPreflightImpl(Request request) const {
  return std::make_unique<ChatRequestPreflightOperation>(
      std::move(request), transcript_, SessionOptions(), ToolDefinitions(), CatalogModel().Info(),
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
                                         std::optional<flFinishReason> backend_finish_reason,
                                         std::optional<BackendTerminationCause> backend_termination) {
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
  if (response.finish_reason == FOUNDRY_LOCAL_FINISH_LENGTH) {
    if (host_output_limit_reached) {
      response.termination_cause = BackendTerminationCause::kOutputTokenLimit;
    } else if (backend_termination.has_value()) {
      response.termination_cause = backend_termination;
    } else if (const int max_output = effective_options.max_output_tokens.value_or(0);
               !backend_finish_reason.has_value() && max_output > 0 && completion_tokens >= max_output) {
      response.termination_cause = BackendTerminationCause::kOutputTokenLimit;
    }
  }

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
  auto prepared = PrepareChatRequest(request, transcript_, SessionOptions(), ToolDefinitions(),
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
  ToolCallRecoveryCheckpoint recovery_checkpoint(prepared->prompt, turn_tool_ctx.tool_call_start_token_id,
                                                 turn_tool_ctx.tool_plan != nullptr && !media_turn);

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
      // Reasoning generators are discarded after each turn, so reserving the full context cannot help continuation.
      const bool use_full_context = backend_kind == ChatBackendKind::kEngine || !tool_ctx.supports_reasoning;
      generator = CreatePreparedTextChatGenerator(prepared_messages, std::move(prepared->prompt), effective_options,
                                                  Model(), tool_ctx, use_full_context, text_generator_factory_);
    }

    prompt_tokens = generator->PromptTokenCount();
    cached_generator_ = std::move(generator);
    cached_tool_ctx_ = std::move(tool_ctx);
    cached_search_options_ = effective_options;
    system_prompt_ = turn_system_prompt;
  }

  const auto host_max_output = prepared->host_max_output_tokens;
  const auto turn_output_budget = host_max_output ? host_max_output : effective_options.max_output_tokens;

  // Generate token-by-token with optional streaming.
  // Check request cancellation each iteration — a streaming callback returning
  // non-zero sets this flag asynchronously via CallbackHandler.
  auto streaming_callback = CreateCallbackHandler(request);
  int output_tokens = 0;
  StopStringFilter stop_filter(effective_options.stop_sequences);
  auto* active_stop_filter =
      effective_options.stop_sequences.empty() ||
              (cached_tool_ctx_.text_output && cached_tool_ctx_.tool_output)
          ? nullptr
          : &stop_filter;
  bool stop_sequence_matched = false;
  bool host_output_limit_reached = false;
  std::vector<GeneratedOutputEvent> generated_events;
  bool semantic_output_seen = false;
  bool malformed_tool_output_seen = false;
  std::string malformed_tool_error;

  // The template opens a new assistant turn, so only calls generated in this reply close its visible text.
  AssistantTurnGuard turn_guard(cached_tool_ctx_.parallel_tool_calls);
  AssistantTextOutputFilter text_framer(cached_tool_ctx_, effective_options);

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
      cached_tool_ctx_.tool_output ? cached_tool_ctx_.GeneratedToolCallStart() : std::string{},
      cached_tool_ctx_.tool_output ? cached_tool_ctx_.GeneratedToolCallEnd() : std::string{},
      cached_tool_ctx_.tools_json,
      cached_tool_ctx_.reasoning_end,
      chat_session_internal::CreateToolCallPayloadParser(cached_tool_ctx_),
      cached_tool_ctx_.tool_plan != nullptr,
      cached_tool_ctx_.fenced_tool_call_output);
  std::optional<RawEnvelopeDetector> raw_detector;
  if (const auto* descriptor = cached_tool_ctx_.ActiveRawEnvelope()) {
    raw_detector.emplace(*descriptor);
  }
  auto* active_raw_detector = raw_detector.has_value() ? &*raw_detector : nullptr;

  auto push_tool_output = [&](const TextSegment& segment) {
    return chat_session_internal::PushToolOutputSegment(
        segment, cached_tool_ctx_, active_raw_detector, tool_accumulator);
  };

  auto emit_visible_text = [&](std::string text) {
    if (text.empty()) {
      return;
    }

    semantic_output_seen = true;
    AppendGeneratedSegment(generated_events, text, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
    if (streaming_callback) {
      streaming_callback->PushItem(std::make_unique<TextItem>(std::move(text), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT));
    }
  };

  auto emit_tool_output = [&](ToolCallStreamAccumulator::Output out) {
    if (malformed_tool_output_seen || text_framer.Matched()) {
      return;
    }

    // The accumulator can emit several calls from one decoded fragment. Admit the batch atomically:
    // no caller-visible item or durable turn state may contain only its valid prefix.
    chat_session_internal::NormalizeToolOutputBatch(out, cached_tool_ctx_);

    for (auto& event : out.events) {
      if (text_framer.Matched()) {
        break;
      }

      if (auto* text = std::get_if<std::string>(&event)) {
        // A structured tool call is the end of the turn's visible text: a chat template renders an assistant turn as
        // content followed by its tool calls, so text produced after a call could only be replayed as though it came
        // before one. Ending the turn here means the caller is never shown output the conversation cannot keep.
        if (!AcceptVisibleText(turn_guard, *text, logger_)) {
          continue;
        }

        emit_visible_text(text_framer.Push(*text));
        stop_sequence_matched |= text_framer.Matched();
        continue;
      }

      emit_visible_text(text_framer.Flush());
      stop_sequence_matched |= text_framer.Matched();
      if (text_framer.Matched()) {
        break;
      }
      text_framer.ValidateBoundary();
      semantic_output_seen = true;
      if (!turn_guard.RecordToolCall()) {
        continue;
      }
      auto call = std::move(std::get<ParsedToolCall>(event));

      if (streaming_callback) {
        streaming_callback->PushItem(std::make_unique<ToolCallItem>(
            call.id, call.name, call.arguments, /*replayed_from_store=*/false,
            cached_tool_ctx_.KindOf(call.name), std::nullopt,
            call.raw_envelope ? GeneratedCallEncoding::kRawEnvelope : GeneratedCallEncoding::kStructured));
      }
      generated_events.push_back(std::move(call));
    }
    malformed_tool_output_seen = out.malformed && !turn_guard.TurnEnded() && !text_framer.Matched();
    if (malformed_tool_output_seen) {
      malformed_tool_error = std::move(out.error);
    }
  };

  auto emit_segments = [&](const std::vector<ReasoningStreamSplitter::Segment>& segments) {
    for (const auto& seg : segments) {
      if (malformed_tool_output_seen || text_framer.Matched() || turn_guard.TurnEnded()) {
        break;
      }

      if (seg.type != FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT) {
        if (active_raw_detector != nullptr && active_raw_detector->HasPendingCandidate()) {
          emit_tool_output(chat_session_internal::AbortRawToolOutput(active_raw_detector));
        }

        if (tool_accumulator.HasPayloadParser()) {
          emit_tool_output(tool_accumulator.RejectPendingSelectedPayload());
        } else if (!tool_accumulator.InsideToolCall()) {
          emit_tool_output(tool_accumulator.Flush());
        }

        if (malformed_tool_output_seen || text_framer.Matched()) {
          break;
        }

        emit_visible_text(text_framer.Flush());
        stop_sequence_matched |= text_framer.Matched();
        if (text_framer.Matched()) {
          break;
        }
        // REASONING goes straight through — never feed it to the tool-call accumulator.
        semantic_output_seen |= !seg.text.empty();
        AppendGeneratedSegment(generated_events, seg.text, seg.type);
        if (streaming_callback) {
          streaming_callback->PushItem(std::make_unique<TextItem>(seg.text, seg.type));
        }
        continue;
      }

      emit_tool_output(push_tool_output(seg));
    }
  };

  auto flush_accumulator = [&](bool natural_end) {
    emit_tool_output(chat_session_internal::FlushToolOutput(
        active_raw_detector, tool_accumulator, natural_end));
  };

  while (!cached_generator_->IsDone() && !request.IsCancellationRequested() && !turn_guard.TurnEnded()) {
    cached_generator_->GenerateNextToken();
    const auto token_id = cached_generator_->CurrentTokenId();
    const bool inside_payload = chat_session_internal::InsideToolOutput(active_raw_detector, tool_accumulator);
    std::string token = RestoreToolBoundary(cached_generator_->Decode(), token_id, cached_tool_ctx_,
                                            splitter.InsideReasoning(), inside_payload);
    recovery_checkpoint.Observe(token_id, token, splitter.InsideReasoning());
    ++output_tokens;

    if (chat_session_internal::PushDecodedFragment(
            token, token_id, active_stop_filter, splitter, emit_segments,
            cached_tool_ctx_.tool_plan && inside_payload) ||
        stop_sequence_matched) {
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
  chat_session_internal::FlushDecodedStream(
      active_stop_filter, splitter, emit_segments,
      cached_tool_ctx_.tool_plan && chat_session_internal::InsideToolOutput(active_raw_detector, tool_accumulator));

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
  emit_visible_text(text_framer.Flush());
  stop_sequence_matched |= text_framer.Matched();
  if (cached_tool_ctx_.tool_plan && !cached_tool_ctx_.text_output &&
      !malformed_tool_output_seen && !HasGeneratedToolCalls(generated_events)) {
    malformed_tool_output_seen = true;
    malformed_tool_error = "The model did not emit the required tool call";
  }
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

  text_framer.ValidateBoundary();
  bool recovered_tool_output = false;
  ToolCallContext completed_tool_ctx;
  if (malformed_tool_output_seen) {
    const bool preserve_prefix = recovery_checkpoint.HasBoundary();
    const bool tool_call_published = HasGeneratedToolCalls(generated_events);
    const int attempted_tokens = std::max(output_tokens, total_tokens - prompt_tokens);
    const bool recovery_eligible = IsGuidedToolRetryEligible(
        natural_tool_output_end, semantic_output_seen,
        effective_options, cached_tool_ctx_, preserve_prefix, tool_call_published, attempted_tokens,
        turn_output_budget);
    if (!recovery_eligible) {
      if (streaming_callback) {
        streaming_callback->Drain();
      }

      ThrowInvalidToolCallWithoutRecovery(malformed_tool_error);
    }

    completed_tool_ctx = cached_tool_ctx_;
    auto retry_tool_ctx = cached_tool_ctx_;
    cached_generator_->Close();
    InvalidateCachedGenerator();

    auto retry_options = effective_options;
    retry_options.max_output_tokens = turn_output_budget;
    if (retry_options.max_output_tokens) {
      *retry_options.max_output_tokens -= attempted_tokens;
    }

    auto retry = RunGuidedToolRetry(
        prepared_messages, retry_options, Model(), std::move(retry_tool_ctx), request,
        text_generator_factory_,
        malformed_tool_error,
        preserve_prefix ? std::optional<PreparedChatPrompt>(recovery_checkpoint.TakePrompt()) : std::nullopt);

    if (!preserve_prefix) {
      generated_events.clear();
    }
    if (!retry.canceled) {
      for (auto& event : retry.events) {
        if (turn_guard.TurnEnded()) {
          break;
        }
        if (auto* segment = std::get_if<TextSegment>(&event)) {
          if (streaming_callback) {
            streaming_callback->PushItem(std::make_unique<TextItem>(segment->text, segment->type));
          }
        } else {
          auto& call = std::get<ParsedToolCall>(event);
          if (!turn_guard.RecordToolCall()) {
            continue;
          }
          if (streaming_callback) {
            streaming_callback->PushItem(std::make_unique<ToolCallItem>(
                call.id, call.name, call.arguments, /*replayed_from_store=*/false,
                completed_tool_ctx.KindOf(call.name), std::nullopt, GeneratedCallEncoding::kStructured));
          }
        }

        generated_events.push_back(std::move(event));
      }
    }

    prompt_tokens += retry.prompt_tokens;
    total_tokens += retry.total_tokens;
    cached_prompt_tokens += retry.cached_prompt_tokens;
    backend_finish_reason = retry.finish_reason;
    backend_termination = retry.termination_cause;
    accepted_reasoning_tokens = preserve_prefix ? accepted_reasoning_tokens + retry.reasoning_tokens
                                                : retry.reasoning_tokens;
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
  if (natural_tool_output_end) {
    text_framer.ValidateCompletion(generated_tool_calls);
  }

  // Reject a generation whose calls cannot be correlated before it reaches the caller — a committed turn must never
  // leave the outstanding-call set inconsistent.
  transcript_.ValidateGeneratedOutput(assistant_message);

  ProcessGeneratedOutput(std::move(generated_events), output_tool_ctx, effective_options,
                         stop_sequence_matched, host_output_limit_reached, response, prompt_tokens, total_tokens,
                         accepted_reasoning_tokens, cached_prompt_tokens, backend_finish_reason, backend_termination);

  // Whole-turn grammar (tool-call-only mode) is a single-shot finite parse. If generation was truncated while grammar
  // was active, the parser is in an unrecoverable state. A completed grammar signals EOS on the next turn. Delimited
  // tool guidance is installed anew on each Engine turn and does not require invalidation after ordinary text.
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
  const bool reasoning_was_active = output_tool_ctx.supports_reasoning;
  const bool discard_after_success = chat_session_internal::ShouldInvalidateRetainedGenerationStateAfterSuccessfulTurn(
      cached_tool_ctx_, !recovered_tool_output && cached_generator_->PromptOpensReasoning(), recovered_tool_output,
      reasoning_was_active, stop_sequence_matched, host_output_limit_reached);

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
  response.openai_json_message_count = prepared.json_message_count;
  std::string completion_id = chat_completions::GenerateCompletionId();
  auto now = std::chrono::system_clock::now();
  int64_t created = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

  const auto& tool_ctx = prepared.tool_context;
  const auto& options = prepared.options;
  ToolCallRecoveryCheckpoint recovery_checkpoint(prepared.prompt, tool_ctx.tool_call_start_token_id,
                                                 tool_ctx.tool_plan != nullptr);
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
  ToolCallStreamAccumulator tool_accumulator(tool_ctx.tool_output ? tool_ctx.GeneratedToolCallStart() : std::string{},
                                             tool_ctx.tool_output ? tool_ctx.GeneratedToolCallEnd() : std::string{},
                                             tool_ctx.tools_json,
                                             tool_ctx.reasoning_end,
                                             chat_session_internal::CreateToolCallPayloadParser(tool_ctx),
                                             tool_ctx.tool_plan != nullptr,
                                             tool_ctx.fenced_tool_call_output);
  std::optional<RawEnvelopeDetector> raw_detector;
  if (const auto* descriptor = tool_ctx.ActiveRawEnvelope()) {
    raw_detector.emplace(*descriptor);
  }
  auto* active_raw_detector = raw_detector.has_value() ? &*raw_detector : nullptr;

  auto push_tool_output = [&](const TextSegment& segment) {
    return chat_session_internal::PushToolOutputSegment(
        segment, tool_ctx, active_raw_detector, tool_accumulator);
  };

  int next_tool_call_index = 0;
  std::vector<GeneratedOutputEvent> generated_events;
  bool semantic_output_seen = false;
  bool malformed_tool_output_seen = false;
  std::string malformed_tool_error;

  AssistantTurnGuard turn_guard(tool_ctx.parallel_tool_calls);
  AssistantTextOutputFilter text_framer(tool_ctx, options);
  bool stop_sequence_matched = false;

  // Use the same typed segments for streaming and final response construction.
  auto splitter = CreateReasoningSplitter(tool_ctx, Model(), generator->PromptOpensReasoning());

  auto emit_visible_text = [&](std::string visible) {
    if (visible.empty()) {
      return;
    }

    semantic_output_seen = true;
    AppendGeneratedSegment(generated_events, visible, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT);
    if (!is_streaming) {
      return;
    }

    auto chunk_json = chat_completions::FormatStreamingChunk(visible, completion_id, created, model_name);
    streaming_callback->PushItem(std::make_unique<TextItem>(std::move(chunk_json),
                                                            FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
  };

  auto process_tool_output = [&](ToolCallStreamAccumulator::Output out) {
    if (malformed_tool_output_seen || text_framer.Matched()) {
      return;
    }

    // Validate and normalize the entire parsed batch before publishing any element. In particular,
    // custom input comes from argument_source, which preserves the complete provider wrapper.
    chat_session_internal::NormalizeToolOutputBatch(out, tool_ctx);

    for (auto& event : out.events) {
      if (text_framer.Matched()) {
        break;
      }

      if (auto* text = std::get_if<std::string>(&event)) {
        // A chat completion carries the assistant reply as `content` plus a `tool_calls` array — the same schema the
        // transcript projection uses, with the same limitation. Text produced after a call would be reported in
        // `content`, in front of the call that actually came first, and a client replaying that message back would
        // be replaying an order of events that never happened. End the turn at the call instead.
        if (!AcceptVisibleText(turn_guard, *text, logger_)) {
          continue;
        }

        emit_visible_text(text_framer.Push(*text));
        stop_sequence_matched |= text_framer.Matched();
        continue;
      }

      emit_visible_text(text_framer.Flush());
      stop_sequence_matched |= text_framer.Matched();
      if (text_framer.Matched()) {
        break;
      }
      text_framer.ValidateBoundary();
      semantic_output_seen = true;
      if (!turn_guard.RecordToolCall()) {
        continue;
      }
      auto call = std::move(std::get<ParsedToolCall>(event));

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
    malformed_tool_output_seen = out.malformed && !turn_guard.TurnEnded() && !text_framer.Matched();
    if (malformed_tool_output_seen) {
      malformed_tool_error = std::move(out.error);
    }
  };

  auto process_segments = [&](const std::vector<ReasoningStreamSplitter::Segment>& segments) {
    for (const auto& seg : segments) {
      if (malformed_tool_output_seen || text_framer.Matched() || turn_guard.TurnEnded()) {
        break;
      }

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

        if (malformed_tool_output_seen || text_framer.Matched()) {
          break;
        }

        emit_visible_text(text_framer.Flush());
        stop_sequence_matched |= text_framer.Matched();
        if (text_framer.Matched()) {
          break;
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

      process_tool_output(push_tool_output(seg));
    }
  };

  // Generate token-by-token.
  StopStringFilter stop_filter(options.stop_sequences);
  auto* active_stop_filter =
      options.stop_sequences.empty() || (tool_ctx.text_output && tool_ctx.tool_output)
          ? nullptr
          : &stop_filter;
  while (!generator->IsDone() && !original_request.IsCancellationRequested() && !turn_guard.TurnEnded()) {
    generator->GenerateNextToken();
    const auto token_id = generator->CurrentTokenId();
    const bool inside_payload = chat_session_internal::InsideToolOutput(active_raw_detector, tool_accumulator);
    std::string token = RestoreToolBoundary(generator->Decode(), token_id, tool_ctx,
                                            splitter.InsideReasoning(), inside_payload);
    recovery_checkpoint.Observe(token_id, token, splitter.InsideReasoning());

    if (chat_session_internal::PushDecodedFragment(
            token, token_id, active_stop_filter, splitter, process_segments,
            tool_ctx.tool_plan && inside_payload) ||
        stop_sequence_matched) {
      stop_sequence_matched = true;
      break;
    }
  }

  // Drain any buffered partial-marker bytes at end-of-stream. Reasoning splitter first so any final DEFAULT bytes
  // feed into the tool accumulator; then drain the tool accumulator.
  chat_session_internal::FlushDecodedStream(
      active_stop_filter, splitter, process_segments,
      tool_ctx.tool_plan && chat_session_internal::InsideToolOutput(active_raw_detector, tool_accumulator));

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
  emit_visible_text(text_framer.Flush());
  stop_sequence_matched |= text_framer.Matched();
  if (tool_ctx.tool_plan && !tool_ctx.text_output &&
      !malformed_tool_output_seen && !HasGeneratedToolCalls(generated_events)) {
    malformed_tool_output_seen = true;
    malformed_tool_error = "The model did not emit the required tool call";
  }
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

  text_framer.ValidateBoundary();
  if (malformed_tool_output_seen) {
    const bool preserve_prefix = recovery_checkpoint.HasBoundary();
    const bool tool_call_published = HasGeneratedToolCalls(generated_events);
    const auto attempted_tokens = total_tokens - prompt_tokens;
    const auto turn_output_budget = prepared.host_max_output_tokens
                                        ? prepared.host_max_output_tokens
                                        : options.max_output_tokens;
    const bool recovery_eligible = IsGuidedToolRetryEligible(
        natural_tool_output_end,
        semantic_output_seen, options, tool_ctx, preserve_prefix, tool_call_published, attempted_tokens,
        turn_output_budget);
    if (!recovery_eligible) {
      if (streaming_callback) {
        streaming_callback->Drain();
      }

      ThrowInvalidToolCallWithoutRecovery(malformed_tool_error);
    }

    generator->Close();
    generator.reset();
    auto retry_options = options;
    retry_options.max_output_tokens = turn_output_budget;
    if (retry_options.max_output_tokens) {
      *retry_options.max_output_tokens -= attempted_tokens;
    }

    auto retry = RunGuidedToolRetry(
        prepared_messages, retry_options, Model(), tool_ctx, original_request,
        text_generator_factory_,
        malformed_tool_error,
        preserve_prefix ? std::optional<PreparedChatPrompt>(recovery_checkpoint.TakePrompt()) : std::nullopt);

    if (!preserve_prefix) {
      generated_events.clear();
    }
    malformed_tool_output_seen = false;
    if (!retry.canceled) {
      for (auto& event : retry.events) {
        if (turn_guard.TurnEnded()) {
          break;
        }
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

    prompt_tokens += retry.prompt_tokens;
    total_tokens += retry.total_tokens;
    cached_prompt_tokens += retry.cached_prompt_tokens;
    backend_finish_reason = retry.finish_reason;
    accepted_reasoning_tokens = preserve_prefix ? accepted_reasoning_tokens + retry.reasoning_tokens
                                                : retry.reasoning_tokens;
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

  if (natural_tool_output_end) {
    text_framer.ValidateCompletion(HasGeneratedToolCalls(generated_events));
  }

  ProcessGeneratedOutput(std::move(generated_events), tool_ctx, options,
                         stop_sequence_matched, /*host_output_limit_reached=*/false, response, prompt_tokens,
                         total_tokens, accepted_reasoning_tokens, cached_prompt_tokens, backend_finish_reason,
                         backend_termination);

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

size_t ChatSession::UndoTurns(size_t count) {
  auto request_lock = LockRequestMutex();

  if (count == 0) {
    return transcript_.TurnCount();
  }

  const bool undo_all = count == transcript_.TurnCount();

  // The transcript validates `count` and rolls back messages, turn records, and outstanding-call state as one step.
  auto tokens = transcript_.UndoTurns(count);

  if (!cached_generator_) {
    return transcript_.TurnCount();
  }

  if (chat_session_internal::ShouldInvalidateRetainedGeneratorForUndo(
          undo_all, tokens.pre_turn.has_value(), cached_generator_->CanRewind())) {
    // Undoing every turn, or undoing back to a turn whose generator was rebuilt — in both cases the current KV cache
    // has no usable boundary matching the target state. A non-rewindable backend must likewise rebuild from the
    // already-truncated transcript.
    InvalidateCachedGenerator();
    return transcript_.TurnCount();
  }

  // The transcript is already truncated. A failed rewind would leave the KV cache describing a conversation the
  // transcript no longer has, so drop the generator before the failure propagates.
  ScopeGuard invalidate_on_failed_rewind([this]() noexcept { InvalidateCachedGenerator(); });
  cached_generator_->RewindTo(*tokens.pre_turn);
  invalidate_on_failed_rewind.Dismiss();
  return transcript_.TurnCount();
}

size_t ChatSession::MessageCount() const {
  return transcript_.MessageCount();
}

}  // namespace fl
