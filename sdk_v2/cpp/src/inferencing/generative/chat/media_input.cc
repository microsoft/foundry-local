// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/media_input.h"

#include "exception.h"
#include "items/text_item.h"

namespace fl {

bool RequestHasMedia(const Request& request) {
  for (const auto* item : request.items) {
    if (item == nullptr || item->type != FOUNDRY_LOCAL_ITEM_MESSAGE) {
      continue;
    }

    const auto& message = static_cast<const MessageItem&>(*item);
    for (const auto& part : message.content) {
      if (part.view && (part.view->type == FOUNDRY_LOCAL_ITEM_IMAGE ||
                        part.view->type == FOUNDRY_LOCAL_ITEM_AUDIO)) {
        return true;
      }
    }
  }

  return false;
}

MediaInput CollectMediaInput(const Request& request) {
  MediaInput media;
  std::vector<const MessageItem*> message_items;

  for (const auto* item : request.items) {
    if (item == nullptr || item->type != FOUNDRY_LOCAL_ITEM_MESSAGE) {
      continue;
    }

    const auto& message_item = static_cast<const MessageItem&>(*item);
    message_items.push_back(&message_item);
    for (const auto& part : message_item.content) {
      if (!part.view) {
        continue;
      }

      if (part.view->type == FOUNDRY_LOCAL_ITEM_IMAGE) {
        media.images.push_back(static_cast<const ImageItem*>(part.view));
      } else if (part.view->type == FOUNDRY_LOCAL_ITEM_AUDIO) {
        media.audios.push_back(static_cast<const AudioItem*>(part.view));
      }
    }
  }

  if (media.Empty()) {
    return media;
  }

  for (const auto* message : message_items) {
    if (!message->content.empty()) {
      media.messages.push_back(*message);
    }
  }

  return media;
}

MediaInput CollectMediaInput(const std::vector<TranscriptMessage>& messages) {
  MediaInput media;
  if (!CarriesMedia(messages)) {
    return media;
  }

  media.messages.reserve(messages.size());
  for (const auto& message : messages) {
    MessageItem projected;
    projected.role = message.role;
    projected.name = message.name;
    for (const auto& entry : message.entries) {
      switch (entry.kind) {
        case TranscriptEntry::Kind::kText:
          projected.content.push_back(MessagePart::Own(std::make_unique<TextItem>(entry.text)));
          break;
        case TranscriptEntry::Kind::kReasoning:
          projected.content.push_back(
              MessagePart::Own(std::make_unique<TextItem>(entry.text, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING)));
          break;
        case TranscriptEntry::Kind::kImage:
          media.images.push_back(static_cast<const ImageItem*>(entry.media.get()));
          projected.content.push_back(MessagePart::Borrow(*entry.media));
          break;
        case TranscriptEntry::Kind::kAudio:
          media.audios.push_back(static_cast<const AudioItem*>(entry.media.get()));
          projected.content.push_back(MessagePart::Borrow(*entry.media));
          break;
        case TranscriptEntry::Kind::kToolCall:
          break;
      }
    }

    media.messages.push_back(std::move(projected));
  }

  return media;
}

void ValidateMediaToolDefinitions(bool tools_declared) {
  if (tools_declared) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "image or audio input cannot be combined with tool definitions; the multimodal prompt does not "
             "support tool calls or results");
  }
}

void ValidateMediaTurn(const MediaInput& media,
                       const std::vector<TranscriptMessage>& inputs,
                       const MediaTurnContext& context) {
  if (media.Empty()) {
    return;
  }

  ValidateMediaToolDefinitions(context.tools_declared);
  if (CarriesToolActivity(inputs)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "image or audio input cannot be combined with tool calls or results in the conversation");
  }
}

}  // namespace fl
