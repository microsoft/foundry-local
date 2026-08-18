// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/preprocessor.h"
#include "exception.h"
#include "util/key_value_pairs.h"

#include <ort_genai.h>

namespace fl {

std::unique_ptr<Preprocessor> Preprocessor::Create(OgaModel& model, bool create_multimodal_processor) {
  std::unique_ptr<OgaTokenizer> tokenizer;
  try {
    tokenizer = OgaTokenizer::Create(model);
  } catch (const std::runtime_error& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to create tokenizer: ", e.what());
  }

  std::unique_ptr<OgaTokenizer> tokenizer_with_special;
  try {
    tokenizer_with_special = OgaTokenizer::Create(model);
    KeyValuePairs options;
    options.Add("skip_special_tokens", "0");
    tokenizer_with_special->UpdateOptions(options.Keys().data(), options.Values().data(), options.size());
  } catch (const std::runtime_error& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to create special-token tokenizer: ", e.what());
  }

  std::unique_ptr<OgaMultiModalProcessor> multimodal_processor;
  if (create_multimodal_processor) {
    try {
      multimodal_processor = OgaMultiModalProcessor::Create(model);
    } catch (const std::runtime_error& e) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to create multimodal processor: ", e.what());
    }
  }

  return std::unique_ptr<Preprocessor>(
      new Preprocessor(std::move(tokenizer), std::move(tokenizer_with_special), std::move(multimodal_processor)));
}

Preprocessor::Preprocessor(std::unique_ptr<OgaTokenizer> tokenizer,
                           std::unique_ptr<OgaTokenizer> tokenizer_with_special,
                           std::unique_ptr<OgaMultiModalProcessor> multimodal_processor)
    : tokenizer_(std::move(tokenizer)),
      tokenizer_with_special_(std::move(tokenizer_with_special)),
      multimodal_processor_(std::move(multimodal_processor)) {
  if (!tokenizer_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "OGA tokenizer is null");
  }
  if (!tokenizer_with_special_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "OGA tokenizer with special tokens is null");
  }
}

Preprocessor::~Preprocessor() = default;

std::unique_ptr<OgaSequences> Preprocessor::Encode(const char* text) {
  auto sequences = OgaSequences::Create();
  std::lock_guard<std::mutex> lock(mutex_);
  tokenizer_->Encode(text, *sequences);
  return sequences;
}

std::string Preprocessor::ApplyChatTemplate(const char* messages_json,
                                            const char* tools_json,
                                            bool add_generation_prompt) {
  std::lock_guard<std::mutex> lock(mutex_);
  OgaString result = tokenizer_->ApplyChatTemplate(/*template_str=*/nullptr, messages_json, tools_json,
                                                   add_generation_prompt);
  return std::string(static_cast<const char*>(result));
}

std::unique_ptr<OgaTokenizerStream> Preprocessor::CreateTokenizerStream() {
  return OgaTokenizerStream::Create(*tokenizer_);
}

std::unique_ptr<OgaTokenizerStream> Preprocessor::CreateSpecialTokenizerStream() {
  return OgaTokenizerStream::Create(*tokenizer_with_special_);
}

const std::vector<int32_t>& Preprocessor::GetEosTokenIds() {
  std::call_once(eos_token_ids_init_flag_, [this]() {
    auto ids = tokenizer_->GetEosTokenIds();
    eos_token_ids_.assign(ids.begin(), ids.end());
  });

  return eos_token_ids_;
}

std::unique_ptr<OgaNamedTensors> Preprocessor::ProcessMedia(const char* prompt,
                                                            const OgaImages* images,
                                                            const OgaAudios* audios) {
  if (!multimodal_processor_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "multimodal processor is null");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (images && audios) {
    return multimodal_processor_->ProcessImagesAndAudios(prompt, images, audios);
  }
  if (images) {
    return multimodal_processor_->ProcessImages(prompt, images);
  }
  return multimodal_processor_->ProcessAudios(prompt, audios);
}

std::unique_ptr<OgaNamedTensors> Preprocessor::ProcessAudios(const std::vector<const char*>& prompts,
                                                             const OgaAudios* audios) {
  if (!multimodal_processor_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "multimodal processor is null");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  return multimodal_processor_->ProcessAudios(prompts, audios);
}

}  // namespace fl
