// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/prepared_chat_prompt.h"

#include "exception.h"
#include "inferencing/generative/chat/chat_template.h"
#include "inferencing/generative/chat/onnx_chat_generator.h"
#include "inferencing/generative/genai_model_instance.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"
#include "items/audio_item.h"
#include "items/image_item.h"

#include <ort_genai.h>

namespace fl {

PreparedChatPrompt::PreparedChatPrompt() = default;
PreparedChatPrompt::~PreparedChatPrompt() = default;
PreparedChatPrompt::PreparedChatPrompt(PreparedChatPrompt&&) noexcept = default;
PreparedChatPrompt& PreparedChatPrompt::operator=(PreparedChatPrompt&&) noexcept = default;

PreparedChatPrompt PrepareTextChatPrompt(const std::vector<TranscriptMessage>& messages,
                                         GenAIModelInstance& model,
                                         const ToolCallContext& tool_ctx) {
  if (messages.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "messages must not be empty");
  }

  PreparedChatPrompt prepared;
  prepared.prompt = BuildChatPrompt(messages, model, tool_ctx.tools_json);
  auto sequences = EncodePrompt(prepared.prompt, model);
  const auto count = sequences->SequenceCount(0);
  const auto* data = sequences->SequenceData(0);
  prepared.token_ids.assign(data, data + count);
  prepared.prompt_token_count = static_cast<int64_t>(count);
  return prepared;
}

PreparedChatPrompt PrepareMediaChatPrompt(const std::vector<MessageItem>& messages,
                                          GenAIModelInstance& model,
                                          const std::vector<const ImageItem*>& images,
                                          const std::vector<const AudioItem*>& audios,
                                          const ToolCallContext& tool_ctx) {
  if (messages.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "messages must not be empty");
  }
  if (images.empty() && audios.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "media preparation requires image or audio input");
  }
  if (!model.IsMultiModal()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "image or audio input requires a multimodal model");
  }
  if (!model.GetPreprocessor().HasMultiModalProcessor()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model has no multimodal processor available for media input");
  }

  PreparedChatPrompt prepared;
  const auto messages_json = OnnxChatGenerator::TransformMessagesForMedia(messages);
  const char* tools = tool_ctx.tools_json.empty() ? nullptr : tool_ctx.tools_json.c_str();
  prepared.prompt = model.GetPreprocessor().ApplyChatTemplate(messages_json.c_str(), tools, true);

  std::unique_ptr<OgaImages> oga_images;
  std::vector<std::vector<uint8_t>> image_bytes;
  if (!images.empty()) {
    image_bytes.reserve(images.size());
    std::vector<const void*> buffers;
    std::vector<size_t> sizes;
    buffers.reserve(images.size());
    sizes.reserve(images.size());

    for (const auto* image : images) {
      if (image == nullptr) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "image entry must not be null");
      }

      image_bytes.push_back(image->ReadBytes());
      const auto& bytes = image_bytes.back();
      buffers.push_back(bytes.data());
      sizes.push_back(bytes.size());
    }

    oga_images = OgaImages::Load(buffers.data(), sizes.data(), buffers.size());
  }

  std::unique_ptr<OgaAudios> oga_audios;
  if (!audios.empty()) {
    std::vector<std::vector<uint8_t>> audio_bytes;
    audio_bytes.reserve(audios.size());
    std::vector<const void*> buffers;
    std::vector<size_t> sizes;
    buffers.reserve(audios.size());
    sizes.reserve(audios.size());

    for (const auto* audio : audios) {
      if (audio == nullptr) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "audio entry must not be null");
      }

      audio_bytes.push_back(audio->ReadBytes());
      const auto& bytes = audio_bytes.back();
      buffers.push_back(bytes.data());
      sizes.push_back(bytes.size());
    }

    oga_audios = OgaAudios::Load(buffers.data(), sizes.data(), buffers.size());
  }

  prepared.media_tensors =
      model.GetPreprocessor().ProcessMedia(prepared.prompt.c_str(), oga_images.get(), oga_audios.get());
  auto input_ids = prepared.media_tensors->Get("input_ids");
  const auto shape = input_ids->Shape();
  if (shape.empty() || shape.back() <= 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "multimodal processor returned invalid input_ids");
  }

  prepared.prompt_token_count = shape.back();
  return prepared;
}

}  // namespace fl
