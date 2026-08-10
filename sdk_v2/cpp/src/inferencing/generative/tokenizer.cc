// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/tokenizer.h"
#include "exception.h"

#include <ort_genai.h>

namespace fl {

Tokenizer::Tokenizer(std::unique_ptr<OgaTokenizer> tokenizer) : tokenizer_(std::move(tokenizer)) {
  if (!tokenizer_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "OGA tokenizer is null");
  }
}

Tokenizer::~Tokenizer() = default;

std::unique_ptr<OgaSequences> Tokenizer::Encode(const char* text) {
  // OgaSequences::Create allocates an independent object; only the shared tokenizer's Encode must be serialized.
  auto sequences = OgaSequences::Create();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tokenizer_->Encode(text, *sequences);
  }

  return sequences;
}

std::string Tokenizer::ApplyChatTemplate(const char* messages_json, const char* tools_json,
                                         bool add_generation_prompt) {
  std::lock_guard<std::mutex> lock(mutex_);
  OgaString result = tokenizer_->ApplyChatTemplate(/*template_str=*/nullptr, messages_json, tools_json,
                                                   add_generation_prompt);

  return std::string(static_cast<const char*>(result));
}

}  // namespace fl
