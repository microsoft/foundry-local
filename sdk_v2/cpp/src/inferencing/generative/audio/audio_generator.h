// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/cancellable.h"

#include <string>

namespace fl {

/// Abstract interface for audio transcription generation.
/// One generator per request — not reusable, not thread-safe.
/// Same pull-based iterator pattern as ChatGenerator:
///   while (!IsDone()) { GenerateNextToken(); text += Decode(); }
class AudioGenerator : public ICancellable {
 public:
  virtual ~AudioGenerator() = default;

  AudioGenerator(const AudioGenerator&) = delete;
  AudioGenerator& operator=(const AudioGenerator&) = delete;

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
  void Cancel() override = 0;

 protected:
  AudioGenerator() = default;
};

}  // namespace fl
