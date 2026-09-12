// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct OgaNamedTensors;

namespace fl {

class AudioItem;
class GenAIModelInstance;
class ImageItem;
class MessageItem;
struct ToolCallContext;
struct TranscriptMessage;

/// Fully rendered and tokenized model input. Construction performs no generation or conversation mutation.
struct PreparedChatPrompt {
  PreparedChatPrompt();
  ~PreparedChatPrompt();
  PreparedChatPrompt(PreparedChatPrompt&&) noexcept;
  PreparedChatPrompt& operator=(PreparedChatPrompt&&) noexcept;

  PreparedChatPrompt(const PreparedChatPrompt&) = delete;
  PreparedChatPrompt& operator=(const PreparedChatPrompt&) = delete;

  std::string prompt;
  std::vector<int32_t> token_ids;
  std::unique_ptr<OgaNamedTensors> media_tensors;
  int64_t prompt_token_count = 0;

  bool HasMedia() const noexcept {
    return media_tensors != nullptr;
  }
};

PreparedChatPrompt PrepareTextChatPrompt(const std::vector<TranscriptMessage>& messages,
                                         GenAIModelInstance& model,
                                         const ToolCallContext& tool_ctx);

PreparedChatPrompt PrepareMediaChatPrompt(const std::vector<MessageItem>& messages,
                                          GenAIModelInstance& model,
                                          const std::vector<const ImageItem*>& images,
                                          const std::vector<const AudioItem*>& audios,
                                          const ToolCallContext& tool_ctx);

}  // namespace fl
