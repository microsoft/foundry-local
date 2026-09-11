// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/types.h"
#include "items/item.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fl {

/// A single tool invocation, carried through the transcript exactly as it was generated or supplied.
struct TranscriptToolCall {
  std::string call_id;
  std::string name;
  /// Raw argument bytes exactly as they crossed the public API boundary. Never rewritten, so the authoritative
  /// record and the response always agree on what actually happened. For a kFunction call these are the JSON
  /// argument bytes; for a kCustom call they are the free-form text payload, already unwrapped from the synthesized
  /// `{"input": ...}` shape the model was prompted with.
  std::string arguments;
  /// Object form used for template projection. Always a JSON object, so projecting a committed transcript can never
  /// fail. For kFunction this is the parsed `arguments`, or an empty object when model output failed to produce one.
  /// For kCustom it is the payload rewrapped as `{"input": arguments}` — the exact shape the model emitted and the
  /// only shape the chat template and the tool-call grammar understand.
  nlohmann::ordered_json normalized_arguments = nlohmann::ordered_json::object();
  /// Which of the session's tool kinds produced this call. Recorded so the authoritative transcript reports the call
  /// as what it was, and so a replayed call normalizes the same way the generated one did.
  ToolKind kind = ToolKind::kFunction;
};

/// One event within a message, stored in the order it occurred.
struct TranscriptEntry {
  enum class Kind {
    kText,       // visible assistant/user text
    kReasoning,  // chain-of-thought text
    kToolCall,   // a tool invocation
  };

  Kind kind = Kind::kText;
  std::string text;
  TranscriptToolCall tool_call;

  static TranscriptEntry Text(std::string value) {
    return {Kind::kText, std::move(value), {}};
  }

  static TranscriptEntry Reasoning(std::string value) {
    return {Kind::kReasoning, std::move(value), {}};
  }

  static TranscriptEntry ToolCall(TranscriptToolCall call) {
    return {Kind::kToolCall, {}, std::move(call)};
  }
};

/// A message in the authoritative transcript.
///
/// Unlike MessageItem (the API-facing type) a transcript message keeps visible text, reasoning, and tool calls as
/// separate ordered events rather than folding everything into modality content parts. Media parts are intentionally
/// absent: media input is single-shot and is fed to the generator directly from the request items.
struct TranscriptMessage {
  flMessageRole role = FOUNDRY_LOCAL_ROLE_NONE;
  std::vector<TranscriptEntry> entries;
  std::string name;  // optional participant name
  /// Set only for role == FOUNDRY_LOCAL_ROLE_TOOL: the assistant call this message answers.
  std::string tool_call_id;

  TranscriptMessage() = default;

  /// Convenience for the common single-text message. An empty `text` produces a message with no entries, which
  /// projects to empty template content — used for assistant turns whose output was entirely hidden reasoning.
  TranscriptMessage(flMessageRole role_in, std::string text, std::string name_in = {});

  /// Build a role="tool" message. An empty `result` is valid and is preserved as empty content.
  static TranscriptMessage ToolResult(std::string call_id, std::string result);

  /// Append text, merging into a trailing entry of the same kind. Empty strings are ignored.
  void AppendText(std::string value);
  void AppendReasoning(std::string value);
  void AppendToolCall(TranscriptToolCall call);

  bool HasToolCalls() const;

  /// True when the message carries visible text that follows a tool call and is more than whitespace.
  ///
  /// This is the one shape a chat template cannot represent. A template renders an assistant turn as content plus a
  /// `tool_calls` array, so text emitted after a call would come back before it — the replayed conversation would
  /// claim a different order of events than the one that actually happened. Whitespace between or after calls says
  /// nothing about order and is not counted.
  ///
  /// See ValidateRenderableTurn for the rule that keeps this false on every committed message.
  bool HasVisibleTextAfterToolCall() const;

  /// Concatenated visible text across all kText entries.
  std::string VisibleText() const;

  /// Concatenated reasoning text across all kReasoning entries.
  std::string ReasoningText() const;

  /// Tool calls in event order.
  std::vector<const TranscriptToolCall*> ToolCalls() const;
};

/// Parse raw function-call argument bytes into the object form used for template projection.
/// Absent arguments mean "no arguments" and yield an empty object. Returns nullopt when bytes exist but are not a
/// JSON object — the caller decides whether that is a client error or a model defect.
std::optional<nlohmann::ordered_json> ParseToolCallArguments(const std::string& arguments);

/// Build a tool call supplied by the caller. Arguments are validated strictly: a client that replays a kFunction
/// call with bytes that are not a JSON object gets an explicit error rather than a silently altered conversation.
/// A kCustom call carries free-form text, so it is always accepted.
///
/// @throws fl::Exception FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT when a kFunction call's arguments are not a JSON
///         object.
TranscriptToolCall MakeSuppliedToolCall(std::string call_id, std::string name, std::string arguments,
                                        ToolKind kind = ToolKind::kFunction);

/// Result of admitting a model-generated tool call into the transcript.
struct GeneratedToolCall {
  TranscriptToolCall call;
  /// False when the model's raw argument bytes were not a JSON object. The raw bytes are preserved on `call` and the
  /// normalized form falls back to an empty object; the caller is expected to report the model defect. Always true
  /// for kCustom calls, whose payload is text and therefore cannot be malformed.
  bool arguments_usable = true;
};

/// Build a tool call from model output.
///
/// Generation has already been streamed to the caller by the time a turn is committed, so a model that emits
/// unusable argument bytes must not fail the request. The raw bytes are preserved verbatim and the normalized form
/// degrades to an empty object, which keeps the committed transcript renderable on every later turn.
GeneratedToolCall MakeGeneratedToolCall(std::string call_id, std::string name, std::string arguments,
                                        ToolKind kind = ToolKind::kFunction);

/// Result of turning a request's items into transcript messages.
struct TranscriptIngest {
  std::vector<TranscriptMessage> messages;

  /// Index into `messages` of the first message the last replay segment produced. Everything before it was
  /// recorded on an earlier turn, so a reply generated now must never merge into it.
  size_t last_segment_start = 0;
};

/// Convert request items into transcript messages, preserving the order in which they were supplied.
///
/// - MESSAGE items become role-tagged messages; typed TextItem parts keep their visible / reasoning distinction and
///   non-text parts are skipped (media is handled separately by the caller). A message with neither content nor a
///   participant name carries nothing and is dropped; a content-free named message survives to attribute the tool
///   calls that follow it.
/// - TOOL_CALL items become assistant messages, merged into an open assistant turn when there is one, so replayed
///   assistant content and its calls stay together exactly as the model produced them.
/// - TOOL_RESULT items become role="tool" messages carrying the call ID. An empty result string is preserved.
/// - Every other item type is rejected. Nothing else can be placed in a conversation, so skipping it would silently
///   drop what the caller sent. Image and audio in particular reach the model as content parts of a MESSAGE item —
///   the only shape the media prompt path renders — so a top-level media item would leave the model answering a
///   question about an image it was never shown.
///
/// @throws fl::Exception FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT for an item type that cannot be placed in a
///         conversation, or for a caller-supplied tool call whose arguments are not a JSON object.
///
/// A contiguous run of assistant items *within one segment* is one assistant turn. One generated turn arrives on
/// replay as separate items — text, then the call it issued — and a live session records all of it on the single
/// message it committed. Merging the run reproduces that message with its events still in the order they were
/// produced, which is also the only shape a chat template can render: a template emits one assistant turn per run.
/// A message that names a different participant is a different speaker and starts a new message; an unnamed message
/// continues the open turn and adopts its name.
///
/// **Behavioural note — consecutive assistant messages are joined, not listed.** A caller that sends two assistant
/// messages in a row (on Chat Completions, or as typed Responses input) gets one assistant message whose text is
/// their literal concatenation, with no separator inserted. This is deliberate continuation semantics, not an
/// accident of the merge: the fragments are two halves of one utterance, exactly as a live session records the text
/// a model emits either side of a tool call, and the caller owns any spacing between them. A generated reply
/// continuing an assistant prefill is joined by the same rule (see ChatTranscript::CommitTurn), so a conversation
/// replayed from storage rebuilds the messages the live session committed.
///
/// `segment_starts` (see Request::item_segment_starts) stops the merge at a recorded-turn boundary: two turns that
/// each ended and began with assistant output stay two messages, exactly as the live session committed them. With
/// no segments the whole item list is one segment and grouping is purely adjacency-based, which is all a caller
/// resending a flat conversation gives us to work with.
///
/// An assistant message whose only content is empty text survives as a message with no entries. That is the
/// assistant-turn boundary of a turn that produced nothing replayable, and dropping it would leave two user turns
/// adjacent in the rebuilt conversation.
///
/// @param tool_kinds Kind of each named tool for this request, snapshotted from the session's registry. A stored
///        call carrying its historical kind uses that value; older stored calls and caller-supplied calls resolve
///        through this map. Names absent from both sources are treated as kFunction.
TranscriptIngest IngestRequestItems(const std::vector<Item*>& items, const std::vector<size_t>& segment_starts,
                                    const std::unordered_map<std::string, ToolKind>& tool_kinds = {});

/// IngestRequestItems for an item list with no known segment boundaries.
std::vector<TranscriptMessage> BuildTranscriptMessages(
    const std::vector<Item*>& items, const std::unordered_map<std::string, ToolKind>& tool_kinds = {});

/// True when any message replays a tool call or answers one.
///
/// Such a turn cannot be appended to an existing generator as a suffix — a chat template renders a tool exchange
/// relative to the surrounding conversation — and the media prompt path cannot render it at all.
bool CarriesToolActivity(const std::vector<TranscriptMessage>& messages);

/// True when `text` is empty or contains nothing but whitespace.
///
/// Shared by the transcript's ordering invariant and by generation. Whitespace after a tool call does not end the
/// turn, but it is dropped because projecting it as visible content would move it before the call.
bool IsWhitespaceOnly(std::string_view text);

/// True when any message is a prior assistant turn or a tool result — the conversation already has history behind
/// whatever the caller is sending now, whether it came from a live transcript or from a replayed chain.
bool CarriesPriorTurnHistory(const std::vector<TranscriptMessage>& messages);

/// True when `messages` carry something the model can actually respond to: visible text, a tool call, or a tool
/// result. Assistant turn boundaries alone are not — they only say a turn happened.
bool CarriesRespondableContent(const std::vector<TranscriptMessage>& messages);

/// Everything outside a turn's messages that can still give the model something to generate from.
struct TurnContent {
  /// The request carries image or audio bytes. They reach the generator directly rather than as message text, so a
  /// media-only turn has no entries at all — the bytes are the question.
  bool media = false;
  /// The session transcript already holds a conversation, so an empty request asks for the next turn of it.
  bool history = false;
  /// The request supplies a system prefix (the Responses API's `instructions`). It is content: a caller may send
  /// instructions alone and expect the model to act on them.
  bool system_prefix = false;
};

/// True when a turn has something to generate from.
///
/// One decision for every way a turn can carry meaning, so an empty `input` behaves the same whether the
/// conversation lives in a warm session or was replayed into the request from storage. A turn with none of them is
/// the caller's mistake and nothing else: it is a client error, never a service failure.
bool TurnCanGenerate(const std::vector<TranscriptMessage>& inputs, const TurnContent& context);

/// The assistant message a reply generated now would merge into, or nullptr when it would start its own message.
///
/// Mirrors CommitTurn's merge rule exactly, so a caller can know before generating whether the reply continues an
/// assistant prefill — and therefore whether that prefill's tool calls already closed the turn's visible text.
const TranscriptMessage* AssistantPrefillForReply(const std::vector<TranscriptMessage>& inputs, size_t merge_floor);

/// Enforce the assistant-turn ordering invariant on one message.
///
/// **Invariant: within an assistant message, all visible text precedes the first tool call.**
///
/// The transcript records events in the order they happened, but the conventional chat-template schema for an
/// assistant turn is `content` plus a `tool_calls` array — it has no way to say "this text came *after* that call".
/// Projecting an interleaved turn would therefore hand the model a different order of events than the one that
/// occurred, silently. Rather than reorder, the invariant makes the shape not exist:
///
///  - Generation treats the first tool call as the end of the turn's visible text. Post-call text is never streamed,
///    never recorded, and stops the turn (ChatSession); the caller sees a `tool_calls` finish reason.
///  - Replayed input carrying that shape is rejected here, because normalizing it would either drop the caller's
///    text or move it, and both are silent changes to what the caller said happened.
///
/// Ordinary `text -> calls` and parallel calls are unaffected: they are exactly what the schema expresses.
///
/// @throws fl::Exception FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT when `message` has visible text after a tool call.
void ValidateRenderableTurn(const TranscriptMessage& message);

/// What generation should do with one visible-text event.
enum class TextDisposition {
  kEmit,     ///< Ordinary visible text: stream it and record it.
  kEndTurn,  ///< The first text that would break the ordering invariant. Drop it and stop generating; report once.
  kDropped,  ///< Whitespace after a call, or any text after the turn ended. Drop it without another report.
};

/// Keeps a turn being generated on the renderable side of the ordering invariant, so a violation is prevented rather
/// than diagnosed after the output has already been streamed to the caller.
///
/// The rule is ValidateRenderableTurn's, applied while events arrive: once the turn has issued a tool call, visible
/// text can no longer be represented, because the chat-template schema would replay it before the call. Such text is
/// dropped and ends the turn — the caller keeps the calls and a `tool_calls` finish reason. Whitespace is dropped
/// without ending the turn, so models can still emit subsequent parallel call blocks without replay reordering it.
///
/// Seeded with the calls of an assistant prefill the reply will merge into (see AssistantPrefillForReply): the
/// prefill and the reply become one message, so the prefill's calls close this turn's visible text too.
class AssistantTurnGuard {
 public:
  explicit AssistantTurnGuard(bool calls_already_issued = false) : calls_issued_(calls_already_issued) {}

  static AssistantTurnGuard ForReplyTo(const std::vector<TranscriptMessage>& inputs, size_t merge_floor);

  /// Record that the turn issued a tool call. Every later visible text event is now unrepresentable.
  void RecordToolCall() noexcept { calls_issued_ = true; }

  /// Decide what to do with one visible-text event. Reports kEndTurn exactly once per turn.
  TextDisposition OfferVisibleText(std::string_view text) noexcept {
    if (ended_) {
      return TextDisposition::kDropped;
    }

    if (!calls_issued_) {
      return TextDisposition::kEmit;
    }

    if (IsWhitespaceOnly(text)) {
      return TextDisposition::kDropped;
    }

    ended_ = true;
    return TextDisposition::kEndTurn;
  }

  /// True once the turn has been ended by post-call text. Generation must stop and the cached generator is no longer
  /// a usable basis for the next turn: the sequence was cut short with no turn terminator.
  bool TurnEnded() const noexcept { return ended_; }

 private:
  bool calls_issued_ = false;
  bool ended_ = false;
};

/// Prepend the turn's system prefix to `messages`, if any.
///
/// The prefix is request state, not conversation history: it never enters the transcript, so it cannot accumulate a
/// copy per turn, and the value the current request supplies is the only one that is ever used.
std::vector<TranscriptMessage> WithSystemPrompt(const std::string& system_prompt,
                                                std::vector<TranscriptMessage> messages);

/// Authoritative ordered record of a conversation.
///
/// The transcript owns two things ChatSession must not duplicate: the committed message order (including tool calls
/// and results) and the outstanding-call bookkeeping used to validate correlation. Turns are committed atomically,
/// so a failed, cancelled, or rejected turn leaves no partial state behind, and undo restores the exact prior state.
class ChatTranscript {
 public:
  enum class CommitPhase {
    kBeforePublish,
    kUndoBeforePublish,
  };
  using CommitFaultInjector = std::function<void(CommitPhase)>;

  explicit ChatTranscript(CommitFaultInjector fault_injector = {})
      : fault_injector_(std::move(fault_injector)) {}

  /// Generator sequence lengths bracketing a turn. Used to rewind the cached generator on undo.
  struct TurnTokens {
    /// Sequence length before this turn's input was appended. Empty when the turn built a fresh generator: the
    /// turn's input is baked into that generator's prompt, so there is no boundary to rewind back to and the
    /// caller must drop the generator instead.
    std::optional<int> pre_turn;
    int post_turn = 0;
  };

  struct Turn {
    size_t message_start = 0;  // index of this turn's first input message
    TurnTokens tokens;
  };

  const std::vector<TranscriptMessage>& Messages() const { return messages_; }
  const std::vector<Turn>& Turns() const { return turns_; }

  size_t MessageCount() const { return messages_.size(); }
  size_t TurnCount() const { return turns_.size(); }
  bool Empty() const { return messages_.empty(); }

  bool IsOutstanding(const std::string& call_id) const { return outstanding_.count(call_id) != 0; }
  size_t OutstandingCallCount() const { return outstanding_.size(); }
  bool HasOutstandingCalls() const { return !outstanding_.empty(); }

  /// Validate a turn's input batch without mutating anything. The batch is walked in order so a replayed assistant
  /// tool call can be answered by a result later in the same batch.
  ///
  /// @throws fl::Exception FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT for a missing, unknown, duplicate, or already
  ///         answered call ID, or for an assistant message that breaks the ordering invariant
  ///         (see ValidateRenderableTurn).
  void ValidateInputs(const std::vector<TranscriptMessage>& inputs) const;

  /// Validate a generated assistant message: every tool call needs a non-empty ID that is unique within the message,
  /// does not collide with an already-issued call, and carries usable arguments. The ordering invariant is checked
  /// too; ChatSession stops generating before it can be broken, so this is the backstop, not the enforcement point.
  ///
  /// @throws fl::Exception FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT
  void ValidateGeneratedOutput(const TranscriptMessage& output) const;

  /// Commit one turn atomically: the input messages followed by the assistant reply. Both are validated first, so a
  /// rejected turn leaves the transcript untouched.
  ///
  /// The reply merges into a trailing assistant input message when the two are one assistant turn: a caller that
  /// supplied assistant content and had the model continue it produced a single assistant turn, and both the
  /// committed record and the prompt must show one. This is the same merge rule item ingestion applies, so a
  /// conversation replayed from storage rebuilds the messages the live session committed.
  ///
  /// `reply_merge_floor` indexes into `inputs` and marks the first input message the reply may merge with.
  /// Everything before it is replayed history from an earlier recorded turn and must stay separate; pass
  /// `inputs.size()` to forbid merging entirely. The default merges with any input, which is what a warm session
  /// wants: every one of its input messages belongs to the turn being committed.
  void CommitTurn(std::vector<TranscriptMessage> inputs, TranscriptMessage output, TurnTokens tokens,
                  size_t reply_merge_floor = 0);

  /// Remove the last `count` turns and restore outstanding-call state to exactly what it was before them.
  ///
  /// Returns the token bracket the caller should rewind its generator to, with no `pre_turn` when the removed range
  /// crosses a turn that rebuilt the generator: the current KV cache's token offsets were established by that
  /// rebuild, so every earlier boundary is stale and the caller must drop the generator instead.
  ///
  /// @throws fl::Exception FOUNDRY_LOCAL_ERROR_INVALID_USAGE if `count` exceeds TurnCount().
  TurnTokens UndoTurns(size_t count);

 private:
  std::vector<TranscriptMessage> messages_;
  std::vector<Turn> turns_;

  // Every call ID ever issued by a committed assistant message, and the subset still awaiting a result. Both are
  // derived state: they exist so validation is O(1) and are rebuilt wholesale on undo.
  std::unordered_set<std::string> issued_;
  std::unordered_set<std::string> outstanding_;
  CommitFaultInjector fault_injector_;
};

}  // namespace fl
