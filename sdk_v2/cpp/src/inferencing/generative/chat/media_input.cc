// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/media_input.h"

#include "exception.h"

namespace fl {

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

void ValidateMediaTurn(const MediaInput& media,
                       const std::vector<TranscriptMessage>& inputs,
                       const MediaTurnContext& context) {
  if (media.Empty()) {
    return;
  }

  if (context.session_has_history || CarriesPriorTurnHistory(inputs)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "image or audio input is only allowed on the first turn of a conversation; the bytes are not part of "
             "the conversation record, so no later prompt can show them to the model again. Start a new "
             "conversation to send media.");
  }

  if (context.tools_declared) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "image or audio input cannot be combined with tool definitions; the model could answer with a tool "
             "call, and the turn carrying that call's result could no longer show it the media. Send the media in a "
             "request that declares no tools.");
  }
}

}  // namespace fl
