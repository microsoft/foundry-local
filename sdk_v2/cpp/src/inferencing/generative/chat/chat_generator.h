// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <string>
#include <optional>
#include <vector>

namespace fl {

class GenAIModelInstance;
struct MessageItem;
struct SearchOptions;

struct ChatTurnUsage {
  int prompt_tokens = 0;
  int generated_tokens = 0;
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
  virtual int AppendMessages(const std::vector<MessageItem>& new_messages,
                             GenAIModelInstance& model,
                             const std::string& tools_json,
                             const SearchOptions& options) = 0;

  /// Returns whether this backend can rewind retained model state directly.
  virtual bool CanRewind() const = 0;

  /// Rewind retained model state to a prior token position.
  virtual void RewindTo(int token_count) = 0;

  /// Return exact usage for the most recently completed turn when the backend exposes it.
  virtual std::optional<ChatTurnUsage> GetTurnUsage() const;

 protected:
  ChatGenerator() = default;
};

}  // namespace fl
