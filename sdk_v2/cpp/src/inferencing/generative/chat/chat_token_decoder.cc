// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/chat_token_decoder.h"

#include "inferencing/generative/genai_model_instance.h"

#include <ort_genai.h>

#include <algorithm>

namespace fl {

ChatTokenDecoder::ChatTokenDecoder(GenAIModelInstance& model)
    : model_(model),
      stream_(model.GetPreprocessor().CreateTokenizerStream()),
      stream_with_special_(model.GetPreprocessor().CreateSpecialTokenizerStream()) {}

ChatTokenDecoder::~ChatTokenDecoder() = default;

std::string ChatTokenDecoder::Decode(int32_t token_id) {
  const char* token_text = stream_->Decode(token_id);
  const char* special_text = stream_with_special_->Decode(token_id);
  std::string token = token_text != nullptr ? token_text : "";

  if (special_text == nullptr || token_text == nullptr || std::string(special_text) == token) {
    return token;
  }

  const std::string special(special_text);
  const bool is_protocol_token =
      special.find("tool_call") != std::string::npos || special.find("think") != std::string::npos;
  const auto& eos_ids = model_.GetPreprocessor().GetEosTokenIds();
  const bool is_eos = std::find(eos_ids.begin(), eos_ids.end(), token_id) != eos_ids.end();

  return is_protocol_token && !is_eos ? special : token;
}

}  // namespace fl
