// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <memory>
#include <mutex>
#include <string>

// Forward declarations for ORT GenAI types (defined in ort_genai.h)
struct OgaTokenizer;
struct OgaSequences;

namespace fl {

/// Owns an ORT GenAI tokenizer and exposes its encode operations.
///
/// A single tokenizer is created per model and shared across all concurrent sessions of that model
/// (see ModelLoadManager). The underlying ort-extensions BPE encode path is not reentrant (it mutates
/// shared pre-tokenizer state), so Encode/ApplyChatTemplate are serialized internally. Callers use this
/// type exactly as they would a plain tokenizer and do not need to know that access is synchronized.
///
/// The decode path (OgaTokenizerStream) is unaffected: each session creates its own stream with its own
/// detokenizer cache, so it operates on Oga() directly without locking.
class Tokenizer {
 public:
  explicit Tokenizer(std::unique_ptr<OgaTokenizer> tokenizer);
  ~Tokenizer();

  Tokenizer(const Tokenizer&) = delete;
  Tokenizer& operator=(const Tokenizer&) = delete;
  Tokenizer(Tokenizer&&) = delete;
  Tokenizer& operator=(Tokenizer&&) = delete;

  /// Encode text into token sequences. Caller takes ownership of the returned sequences.
  std::unique_ptr<OgaSequences> Encode(const char* text);

  /// Render the model's built-in chat template for the given messages and (optional) tools JSON.
  std::string ApplyChatTemplate(const char* messages_json, const char* tools_json, bool add_generation_prompt);

  /// Access the underlying OgaTokenizer for operations that do not touch the mutable encode state:
  /// creating per-session decode streams and reading immutable metadata (e.g. EOS token ids).
  OgaTokenizer& Oga() { return *tokenizer_; }

 private:
  std::unique_ptr<OgaTokenizer> tokenizer_;
  std::mutex mutex_;
};

}  // namespace fl
