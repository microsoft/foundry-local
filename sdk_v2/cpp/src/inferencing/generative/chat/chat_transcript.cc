// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/chat_transcript.h"

#include "exception.h"
#include "inferencing/session/tool_registry.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
#include <utility>

namespace fl {

namespace {

/// Concatenate the text of every entry of `kind`. Parts are literal fragments — the producer owns any separators.
std::string JoinEntries(const std::vector<TranscriptEntry>& entries, TranscriptEntry::Kind kind) {
  std::string text;
  for (const auto& entry : entries) {
    if (entry.kind == kind) {
      text += entry.text;
    }
  }

  return text;
}

}  // namespace

bool IsWhitespaceOnly(std::string_view text) {
  return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

// ---------------------------------------------------------------------------
// Tool call arguments
// ---------------------------------------------------------------------------

std::optional<nlohmann::ordered_json> ParseToolCallArguments(const std::string& arguments) {
  if (arguments.empty()) {
    return nlohmann::ordered_json::object();
  }

  auto parsed = nlohmann::ordered_json::parse(arguments, nullptr, /*allow_exceptions=*/false);
  if (!parsed.is_object()) {
    return std::nullopt;
  }

  return parsed;
}

/// Project raw argument bytes into the object form the chat template renders, according to the tool's kind.
///
/// kFunction defers to ParseToolCallArguments and can fail. kCustom never fails: the payload is free-form text by
/// definition, so it is wrapped as `{"input": <arguments>}` — the exact shape the synthesized custom-tool schema
/// asks the model for, and therefore the shape a replayed call must render back to. Wrapping unconditionally keeps
/// the rule unambiguous: a payload that happens to look like `{"input": "..."}` is still just text.
static std::optional<nlohmann::ordered_json> NormalizeToolCallArguments(ToolKind kind,
                                                                        const std::string& arguments) {
  if (kind == ToolKind::kCustom) {
    ValidateToolCallText(arguments, "custom tool payload");
    return nlohmann::ordered_json{{kCustomToolInputParameter, arguments}};
  }

  return ParseToolCallArguments(arguments);
}

TranscriptToolCall MakeSuppliedToolCall(std::string call_id, std::string name, std::string arguments, ToolKind kind) {
  auto normalized = NormalizeToolCallArguments(kind, arguments);
  if (!normalized.has_value()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "tool call '" + name + "' has arguments that are not a JSON object: " + arguments);
  }

  return {std::move(call_id), std::move(name), std::move(arguments), std::move(*normalized), kind};
}

GeneratedToolCall MakeGeneratedToolCall(std::string call_id, std::string name, std::string arguments, ToolKind kind) {
  auto normalized = NormalizeToolCallArguments(kind, arguments);
  const bool usable = normalized.has_value();

  return {{std::move(call_id), std::move(name), std::move(arguments),
           usable ? std::move(*normalized) : nlohmann::ordered_json::object(), kind},
          usable};
}

// ---------------------------------------------------------------------------
// TranscriptMessage
// ---------------------------------------------------------------------------

TranscriptMessage::TranscriptMessage(flMessageRole role_in, std::string text, std::string name_in)
    : role(role_in), name(std::move(name_in)) {
  AppendText(std::move(text));
}

TranscriptMessage TranscriptMessage::ToolResult(std::string call_id, std::string result) {
  TranscriptMessage message;
  message.role = FOUNDRY_LOCAL_ROLE_TOOL;
  message.tool_call_id = std::move(call_id);
  message.AppendText(std::move(result));
  return message;
}

void TranscriptMessage::AppendText(std::string value) {
  if (value.empty()) {
    return;
  }

  if (!entries.empty() && entries.back().kind == TranscriptEntry::Kind::kText) {
    entries.back().text += value;
    return;
  }

  entries.push_back(TranscriptEntry::Text(std::move(value)));
}

void TranscriptMessage::AppendReasoning(std::string value) {
  if (value.empty()) {
    return;
  }

  if (!entries.empty() && entries.back().kind == TranscriptEntry::Kind::kReasoning) {
    entries.back().text += value;
    return;
  }

  entries.push_back(TranscriptEntry::Reasoning(std::move(value)));
}

void TranscriptMessage::AppendToolCall(TranscriptToolCall call) {
  entries.push_back(TranscriptEntry::ToolCall(std::move(call)));
}

bool TranscriptMessage::HasToolCalls() const {
  for (const auto& entry : entries) {
    if (entry.kind == TranscriptEntry::Kind::kToolCall) {
      return true;
    }
  }

  return false;
}

bool TranscriptMessage::HasVisibleTextAfterToolCall() const {
  bool seen_call = false;
  for (const auto& entry : entries) {
    if (entry.kind == TranscriptEntry::Kind::kToolCall) {
      seen_call = true;
      continue;
    }

    if (seen_call && entry.kind == TranscriptEntry::Kind::kText && !IsWhitespaceOnly(entry.text)) {
      return true;
    }
  }

  return false;
}

std::string TranscriptMessage::VisibleText() const {
  return JoinEntries(entries, TranscriptEntry::Kind::kText);
}

std::string TranscriptMessage::ReasoningText() const {
  return JoinEntries(entries, TranscriptEntry::Kind::kReasoning);
}

std::vector<const TranscriptToolCall*> TranscriptMessage::ToolCalls() const {
  std::vector<const TranscriptToolCall*> calls;
  for (const auto& entry : entries) {
    if (entry.kind == TranscriptEntry::Kind::kToolCall) {
      calls.push_back(&entry.tool_call);
    }
  }

  return calls;
}

// ---------------------------------------------------------------------------
// Item ingestion
// ---------------------------------------------------------------------------

namespace {

/// True when `candidate` continues the assistant turn `open` already started. A different participant name is a
/// different speaker and therefore a different message; an unnamed message continues the open turn.
bool ContinuesAssistantTurn(const TranscriptMessage& open, const TranscriptMessage& candidate) {
  if (open.role != FOUNDRY_LOCAL_ROLE_ASSISTANT || candidate.role != FOUNDRY_LOCAL_ROLE_ASSISTANT) {
    return false;
  }

  return open.name.empty() || candidate.name.empty() || open.name == candidate.name;
}

/// Append every event of `next` to `open`, keeping the order the events were produced in. Text and reasoning runs
/// coalesce with a trailing run of the same kind, exactly as they do while a turn is being generated. An open turn
/// with no name adopts the continuation's name, so a named speaker is never lost.
///
/// Coalescing is literal: no separator is inserted. Two assistant fragments are two halves of one utterance — the
/// live session records "Let me" and " check." as the single message "Let me check." because that is what the model
/// emitted — so any spacing belongs to the fragments themselves. Inserting one here would put text in the
/// conversation that nobody produced, and would make a replayed turn differ from the turn it replays.
void MergeAssistantTurn(TranscriptMessage& open, TranscriptMessage&& next) {
  if (open.name.empty()) {
    open.name = std::move(next.name);
  }

  for (auto& entry : next.entries) {
    switch (entry.kind) {
      case TranscriptEntry::Kind::kText:
        if (open.HasToolCalls() && IsWhitespaceOnly(entry.text)) {
          break;
        }
        open.AppendText(std::move(entry.text));
        break;
      case TranscriptEntry::Kind::kReasoning:
        open.AppendReasoning(std::move(entry.text));
        break;
      case TranscriptEntry::Kind::kToolCall:
        open.AppendToolCall(std::move(entry.tool_call));
        break;
    }
  }
}

/// The one merge rule, shared by item ingestion and turn commit.
///
/// Appends `message`, merging it into the trailing message when the two are one assistant turn. `merge_floor` is
/// the first index of the current segment: nothing before it may be merged into, so two recorded turns can never
/// collapse into a single message.
void AppendWithinSegment(std::vector<TranscriptMessage>& messages, TranscriptMessage&& message, size_t merge_floor) {
  if (messages.size() > merge_floor && ContinuesAssistantTurn(messages.back(), message)) {
    MergeAssistantTurn(messages.back(), std::move(message));
    return;
  }

  messages.push_back(std::move(message));
}

}  // namespace

TranscriptIngest IngestRequestItems(const std::vector<Item*>& items, const std::vector<size_t>& segment_starts,
                                    const std::unordered_map<std::string, ToolKind>& tool_kinds) {
  // A replayed call is normalized the way the generated one was: a custom tool's payload is text, so resolving its
  // kind here is what keeps it from being rejected as malformed JSON on the way back in.
  const auto kind_of = [&tool_kinds](const std::string& name) {
    auto it = tool_kinds.find(name);
    return it == tool_kinds.end() ? ToolKind::kFunction : it->second;
  };

  TranscriptIngest ingest;
  auto& messages = ingest.messages;

  auto next_boundary = segment_starts.begin();

  for (size_t index = 0; index < items.size(); ++index) {
    // Close the open assistant turn whenever a new recorded segment begins. Several boundaries can land on the same
    // index when a segment contributes no items at all.
    while (next_boundary != segment_starts.end() && *next_boundary <= index) {
      ingest.last_segment_start = messages.size();
      ++next_boundary;
    }

    const auto* item = items[index];
    if (item == nullptr) {
      continue;
    }

    if (item->type == FOUNDRY_LOCAL_ITEM_MESSAGE) {
      const auto& message_item = static_cast<const MessageItem&>(*item);

      // A message with no content parts carries nothing to say — unless it names the participant, in which case it
      // exists to attribute the tool calls that follow it.
      if (message_item.content.empty() && message_item.name.empty()) {
        continue;
      }

      TranscriptMessage message;
      message.role = message_item.role;
      message.name = message_item.name;

      for (const auto& part : message_item.content) {
        if (!part.view || part.view->type != FOUNDRY_LOCAL_ITEM_TEXT) {
          continue;
        }

        const auto& text_item = static_cast<const TextItem&>(*part.view);
        if (text_item.text_type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING) {
          message.AppendReasoning(text_item.text);
        } else {
          message.AppendText(text_item.text);
        }
      }

      AppendWithinSegment(messages, std::move(message), ingest.last_segment_start);
      continue;
    }

    if (item->type == FOUNDRY_LOCAL_ITEM_TOOL_CALL) {
      const auto& call_item = static_cast<const ToolCallItem&>(*item);

      TranscriptMessage message;
      message.role = FOUNDRY_LOCAL_ROLE_ASSISTANT;
      if (call_item.replayed_from_store) {
        const auto kind = call_item.replayed_kind.value_or(kind_of(call_item.name));
        message.AppendToolCall(MakeGeneratedToolCall(call_item.call_id, call_item.name,
                                                     call_item.replayed_arguments, kind)
                                   .call);
      } else {
        const auto kind = call_item.declared_kind.value_or(kind_of(call_item.name));
        message.AppendToolCall(
            MakeSuppliedToolCall(call_item.call_id, call_item.name, call_item.arguments, kind));
      }

      // The same rule folds the call into the open assistant turn, so replayed content and its calls stay in one
      // message — and a call that opens a segment starts its own.
      AppendWithinSegment(messages, std::move(message), ingest.last_segment_start);
      continue;
    }

    if (item->type == FOUNDRY_LOCAL_ITEM_TOOL_RESULT) {
      const auto& result_item = static_cast<const ToolResultItem&>(*item);
      messages.push_back(TranscriptMessage::ToolResult(result_item.call_id, result_item.result));
      continue;
    }

    // Nothing else belongs in a conversation, and skipping it would silently drop what the caller sent — the turn
    // would answer a question the prompt never carried. Media is the likely mistake: image and audio reach the model
    // as content parts of a message item, which is the only shape CollectMediaInput gathers and the only one the
    // media prompt path renders.
    if (item->type == FOUNDRY_LOCAL_ITEM_IMAGE || item->type == FOUNDRY_LOCAL_ITEM_AUDIO) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "image and audio input must be content parts of a message item; a top-level media item cannot be "
               "placed in the conversation and would never reach the model");
    }

    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "a conversation accepts only message, tool call, and tool result items; anything else would be "
             "dropped without reaching the model");
  }

  // Boundaries past the last item still open a (empty) final segment: a hop that contributed nothing must not let
  // the next reply merge into the hop before it.
  if (next_boundary != segment_starts.end()) {
    ingest.last_segment_start = messages.size();
  }

  return ingest;
}

std::vector<TranscriptMessage> BuildTranscriptMessages(
    const std::vector<Item*>& items, const std::unordered_map<std::string, ToolKind>& tool_kinds) {
  return IngestRequestItems(items, {}, tool_kinds).messages;
}

bool CarriesToolActivity(const std::vector<TranscriptMessage>& messages) {
  return std::any_of(messages.begin(), messages.end(), [](const TranscriptMessage& message) {
    return message.role == FOUNDRY_LOCAL_ROLE_TOOL || message.HasToolCalls();
  });
}

bool CarriesPriorTurnHistory(const std::vector<TranscriptMessage>& messages) {
  return std::any_of(messages.begin(), messages.end(), [](const TranscriptMessage& message) {
    return message.role == FOUNDRY_LOCAL_ROLE_ASSISTANT || message.role == FOUNDRY_LOCAL_ROLE_TOOL;
  });
}

bool CarriesRespondableContent(const std::vector<TranscriptMessage>& messages) {
  return std::any_of(messages.begin(), messages.end(), [](const TranscriptMessage& message) {
    return !message.VisibleText().empty() || message.HasToolCalls() || message.role == FOUNDRY_LOCAL_ROLE_TOOL;
  });
}

bool TurnCanGenerate(const std::vector<TranscriptMessage>& inputs, const TurnContent& context) {
  return context.media || context.history || context.system_prefix || CarriesPriorTurnHistory(inputs) ||
         CarriesRespondableContent(inputs);
}

const TranscriptMessage* AssistantPrefillForReply(const std::vector<TranscriptMessage>& inputs, size_t merge_floor) {
  if (inputs.size() <= merge_floor || inputs.back().role != FOUNDRY_LOCAL_ROLE_ASSISTANT) {
    return nullptr;
  }

  // A generated reply has no participant name, so it continues whatever name the prefill carries.
  return &inputs.back();
}

AssistantTurnGuard AssistantTurnGuard::ForReplyTo(const std::vector<TranscriptMessage>& inputs, size_t merge_floor) {
  const auto* prefill = AssistantPrefillForReply(inputs, merge_floor);
  return AssistantTurnGuard(prefill != nullptr && prefill->HasToolCalls());
}

void ValidateRenderableTurn(const TranscriptMessage& message) {
  if (!message.HasVisibleTextAfterToolCall()) {
    return;
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
           "an assistant turn cannot carry visible text after a tool call: a chat template renders an assistant "
           "turn as content followed by its tool calls, so replaying that text would present it to the model as "
           "though it came before the call. Split the text and the call into separate turns, or send the text "
           "before the call.");
}

std::vector<TranscriptMessage> WithSystemPrompt(const std::string& system_prompt,
                                                std::vector<TranscriptMessage> messages) {
  if (system_prompt.empty()) {
    return messages;
  }

  std::vector<TranscriptMessage> prefixed;
  prefixed.reserve(messages.size() + 1);
  prefixed.emplace_back(FOUNDRY_LOCAL_ROLE_SYSTEM, system_prompt);
  for (auto& message : messages) {
    prefixed.push_back(std::move(message));
  }

  return prefixed;
}

// ---------------------------------------------------------------------------
// ChatTranscript
// ---------------------------------------------------------------------------

namespace {

/// Validate one assistant message and record the calls it issues. The ordering invariant is checked first: a message
/// that cannot be rendered must be rejected before any of its calls become outstanding. `issued` and `outstanding`
/// may be the transcript's own sets or scratch copies used for a dry run.
void StageAssistantMessage(const TranscriptMessage& message,
                           std::unordered_set<std::string>& issued,
                           std::unordered_set<std::string>& outstanding) {
  ValidateRenderableTurn(message);

  for (const auto* call : message.ToolCalls()) {
    if (call->call_id.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "tool call requires a non-empty call id");
    }

    if (!issued.insert(call->call_id).second) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "tool call id '" + call->call_id + "' is already used by another tool call");
    }

    outstanding.insert(call->call_id);
  }
}

/// Validate a role="tool" message against the outstanding calls and consume the call it answers.
void StageToolResult(const TranscriptMessage& message,
                     const std::unordered_set<std::string>& issued,
                     std::unordered_set<std::string>& outstanding) {
  if (message.tool_call_id.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "tool result requires a non-empty call id");
  }

  if (outstanding.erase(message.tool_call_id) == 1) {
    return;
  }

  if (issued.count(message.tool_call_id) != 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "tool call id '" + message.tool_call_id + "' already has a result");
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
           "tool result references unknown tool call id '" + message.tool_call_id + "'");
}

/// Replay a turn against call-ID state, validating as it goes. The batch is walked in order because a single turn may
/// both replay an assistant call and answer it. `output` is null when only the input batch is being checked.
void StageTurn(const std::vector<TranscriptMessage>& inputs,
               const TranscriptMessage* output,
               std::unordered_set<std::string>& issued,
               std::unordered_set<std::string>& outstanding) {
  for (const auto& message : inputs) {
    if (message.role == FOUNDRY_LOCAL_ROLE_TOOL) {
      StageToolResult(message, issued, outstanding);
    } else if (message.role == FOUNDRY_LOCAL_ROLE_ASSISTANT) {
      StageAssistantMessage(message, issued, outstanding);
    }
  }

  if (output != nullptr) {
    StageAssistantMessage(*output, issued, outstanding);
  }
}

}  // namespace

void ChatTranscript::ValidateInputs(const std::vector<TranscriptMessage>& inputs) const {
  auto issued = issued_;
  auto outstanding = outstanding_;
  StageTurn(inputs, nullptr, issued, outstanding);
}

void ChatTranscript::ValidateGeneratedOutput(const TranscriptMessage& output) const {
  auto issued = issued_;
  auto outstanding = outstanding_;
  StageTurn({}, &output, issued, outstanding);
}

void ChatTranscript::CommitTurn(std::vector<TranscriptMessage> inputs, TranscriptMessage output, TurnTokens tokens,
                                size_t reply_merge_floor) {
  // Build every part of the post-commit state separately. Validation, copying, vector growth, assistant merging, and
  // call-state updates may all throw; none of them may partially publish a turn.
  auto messages = messages_;
  auto turns = turns_;
  auto issued = issued_;
  auto outstanding = outstanding_;
  StageTurn(inputs, &output, issued, outstanding);

  // A reply that merges into a trailing assistant prefill becomes one message with it, so the ordering invariant
  // applies to the merged result rather than to the two halves. Checked before anything is appended, so a rejected
  // turn still leaves the transcript untouched.
  if (const auto* prefill = AssistantPrefillForReply(inputs, reply_merge_floor);
      prefill != nullptr && ContinuesAssistantTurn(*prefill, output)) {
    TranscriptMessage merged = *prefill;
    auto reply = output;
    MergeAssistantTurn(merged, std::move(reply));
    ValidateRenderableTurn(merged);
  }

  Turn turn;
  turn.message_start = messages.size();
  turn.tokens = tokens;

  // The floor is expressed against `inputs`; translate it to an index in the committed list. Inputs are appended
  // as ingestion produced them — it already applied the merge rule within each segment it knew about.
  const size_t floor = turn.message_start + std::min(reply_merge_floor, inputs.size());

  messages.reserve(messages.size() + inputs.size() + 1);
  for (auto& message : inputs) {
    messages.push_back(std::move(message));
  }

  AppendWithinSegment(messages, std::move(output), floor);
  turns.push_back(turn);

  if (fault_injector_) {
    fault_injector_(CommitPhase::kBeforePublish);
  }

  messages_.swap(messages);
  turns_.swap(turns);
  issued_.swap(issued);
  outstanding_.swap(outstanding);
}

ChatTranscript::TurnTokens ChatTranscript::UndoTurns(size_t count) {
  if (count > turns_.size()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "Cannot undo " + std::to_string(count) + " turns; only " + std::to_string(turns_.size()) +
                 " turns exist");
  }

  if (count == 0) {
    return {};
  }

  const size_t first_removed = turns_.size() - count;
  TurnTokens tokens = turns_[first_removed].tokens;

  // Any rebuilt turn inside the removed range reset the generator's token scale, so the target turn's boundary no
  // longer refers to the current KV cache. Report no rewind point and let the caller invalidate.
  for (size_t i = first_removed; i < turns_.size(); ++i) {
    if (!turns_[i].tokens.pre_turn.has_value()) {
      tokens.pre_turn.reset();
      break;
    }
  }

  // Rebuild rather than unwind: undone tool calls must stop being answerable, and a full recompute is the only
  // representation that cannot drift from the retained messages. Every potentially throwing operation works on
  // scratch state; publication is a set of nonthrowing swaps.
  auto messages = messages_;
  messages.resize(turns_[first_removed].message_start);

  auto turns = turns_;
  turns.resize(first_removed);

  std::unordered_set<std::string> issued;
  std::unordered_set<std::string> outstanding;
  StageTurn(messages, nullptr, issued, outstanding);

  if (fault_injector_) {
    fault_injector_(CommitPhase::kUndoBeforePublish);
  }

  messages_.swap(messages);
  turns_.swap(turns);
  issued_.swap(issued);
  outstanding_.swap(outstanding);

  return tokens;
}

}  // namespace fl
