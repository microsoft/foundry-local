// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "foundry_local/foundry_local_c.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fl {

class GenAIModelInstance;
struct PreparedChatPrompt;
struct TranscriptMessage;
struct SearchOptions;
struct ToolCallContext;

struct ChatTurnUsage {
  int prompt_tokens = 0;
  int generated_tokens = 0;
  std::optional<flFinishReason> finish_reason;
};

class RetainedPromptMismatchError : public std::runtime_error {
 public:
  RetainedPromptMismatchError() : std::runtime_error("retained tokens are not a prefix of the full prompt") {}
};

/// Abstract interface for token-by-token text generation.
/// One generator per request — not reusable, not thread-safe.
/// Follows the classic pull-based iterator pattern:
///   while (!IsDone()) { GenerateNextToken(); text += Decode(); }
class ChatGenerator {
 public:
  virtual ~ChatGenerator() = default;

  ChatGenerator(const ChatGenerator&) = delete;
  ChatGenerator& operator=(const ChatGenerator&) = delete;

  /// Returns true when generation is complete (EOS token, max_length, or stop condition).
  virtual bool IsDone() const = 0;

  /// Generate the next token. Must not be called after IsDone() returns true.
  virtual void GenerateNextToken() = 0;

  /// Decode the most recently generated token into text.
  /// Returns empty string for special/control tokens that should not be surfaced.
  virtual std::string Decode() = 0;

  /// Get the most recently generated token ID before Decode consumes it, when exposed by the backend.
  virtual std::optional<int32_t> CurrentTokenId() const = 0;

  /// Get the total number of tokens (input + generated) so far.
  virtual int TokenCount() const = 0;

  /// Get the number of prompt (input) tokens.
  virtual int PromptTokenCount() const = 0;

  /// Convenience: generate all tokens and return the full decoded text.
  /// Default implementation loops GenerateNextToken/Decode.
  virtual std::string GenerateAll();

  /// Request cancellation of generation. Thread-safe — can be called from another thread.
  /// After cancellation, IsDone() should return true on the next check.
  virtual void Cancel() = 0;

  /// Append a new conversational turn to retained model state.
  ///
  /// full_messages contains the complete structured transcript through new_messages. Backends that reconcile
  /// retained tokens against a freshly rendered prompt use it to decide whether the retained state is reusable.
  virtual int AppendMessages(const std::vector<TranscriptMessage>& new_messages,
                             const std::vector<TranscriptMessage>& full_messages,
                             GenAIModelInstance& model,
                             const ToolCallContext& tool_ctx,
                             const SearchOptions& options) = 0;

  /// Append the exact full-prompt artifact produced during request preparation.
  virtual int AppendPreparedPrompt(const std::vector<TranscriptMessage>& new_messages,
                                   const PreparedChatPrompt& prepared,
                                   GenAIModelInstance& model,
                                   const ToolCallContext& tool_ctx,
                                   const SearchOptions& options) = 0;

  /// Whether the prompt for the active turn ends inside a reasoning block opened by the chat template.
  virtual bool PromptOpensReasoning() const { return false; }

  /// Returns whether this backend can rewind retained model state directly.
  virtual bool CanRewind() const { return false; }

  /// Rewind retained model state to a prior token position.
  virtual void RewindTo(int token_count);

  /// Return exact usage for the most recently completed turn when the backend exposes it.
  virtual std::optional<ChatTurnUsage> GetTurnUsage() const;

 protected:
  ChatGenerator() = default;
};

}  // namespace fl
