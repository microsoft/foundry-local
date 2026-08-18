// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct OgaAudios;
struct OgaImages;
struct OgaModel;
struct OgaMultiModalProcessor;
struct OgaNamedTensors;
struct OgaSequences;
struct OgaTokenizer;
struct OgaTokenizerStream;

namespace fl {

/// Owns model preprocessing resources and serializes operations that use non-reentrant tokenizer state.
class Preprocessor {
 public:
  static std::unique_ptr<Preprocessor> Create(OgaModel& model, bool create_multimodal_processor);

  ~Preprocessor();

  Preprocessor(const Preprocessor&) = delete;
  Preprocessor& operator=(const Preprocessor&) = delete;
  Preprocessor(Preprocessor&&) = delete;
  Preprocessor& operator=(Preprocessor&&) = delete;

  std::unique_ptr<OgaSequences> Encode(const char* text);
  std::string ApplyChatTemplate(const char* messages_json, const char* tools_json, bool add_generation_prompt);

  std::unique_ptr<OgaTokenizerStream> CreateTokenizerStream();
  std::unique_ptr<OgaTokenizerStream> CreateSpecialTokenizerStream();
  const std::vector<int32_t>& GetEosTokenIds();

  bool HasMultiModalProcessor() const { return multimodal_processor_ != nullptr; }
  std::unique_ptr<OgaNamedTensors> ProcessMedia(const char* prompt,
                                                const OgaImages* images,
                                                const OgaAudios* audios);
  std::unique_ptr<OgaNamedTensors> ProcessAudios(const std::vector<const char*>& prompts,
                                                 const OgaAudios* audios);

 private:
  Preprocessor(std::unique_ptr<OgaTokenizer> tokenizer,
               std::unique_ptr<OgaTokenizer> tokenizer_with_special,
               std::unique_ptr<OgaMultiModalProcessor> multimodal_processor);

  std::mutex mutex_;
  std::unique_ptr<OgaTokenizer> tokenizer_;
  std::unique_ptr<OgaTokenizer> tokenizer_with_special_;
  std::unique_ptr<OgaMultiModalProcessor> multimodal_processor_;
  std::vector<int32_t> eos_token_ids_;
  std::once_flag eos_token_ids_init_flag_;
};

}  // namespace fl
