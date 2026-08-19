// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct OgaTokenizerStream;

namespace fl {

class GenAIModelInstance;

/// Decodes generated tokens consistently across generator and engine-backed chat paths.
class ChatTokenDecoder final {
 public:
  explicit ChatTokenDecoder(GenAIModelInstance& model);
  ~ChatTokenDecoder();

  ChatTokenDecoder(const ChatTokenDecoder&) = delete;
  ChatTokenDecoder& operator=(const ChatTokenDecoder&) = delete;

  std::string Decode(int32_t token_id);

 private:
  GenAIModelInstance& model_;
  std::unique_ptr<OgaTokenizerStream> stream_;
  std::unique_ptr<OgaTokenizerStream> stream_with_special_;
};

}  // namespace fl
