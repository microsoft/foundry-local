// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/session/request.h"

#include "exception.h"
#include "items/audio_item.h"
#include "items/image_item.h"
#include "items/message_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"

#include <cstdint>

namespace fl {
namespace {

std::unique_ptr<Item> CloneChatItem(const Item& item) {
  switch (item.type) {
    case FOUNDRY_LOCAL_ITEM_TEXT: {
      const auto& text = static_cast<const TextItem&>(item);
      return std::make_unique<TextItem>(text.text, text.text_type);
    }
    case FOUNDRY_LOCAL_ITEM_MESSAGE:
      return std::make_unique<MessageItem>(static_cast<const MessageItem&>(item));
    case FOUNDRY_LOCAL_ITEM_TOOL_CALL: {
      const auto& call = static_cast<const ToolCallItem&>(item);
      auto clone = std::make_unique<ToolCallItem>(call.call_id, call.name, call.arguments, call.replayed_from_store,
                                                  call.kind, call.declared_kind, call.generated_encoding);
      clone->replayed_arguments = call.replayed_arguments;
      clone->replayed_kind = call.replayed_kind;
      return clone;
    }
    case FOUNDRY_LOCAL_ITEM_TOOL_RESULT: {
      const auto& result = static_cast<const ToolResultItem&>(item);
      return std::make_unique<ToolResultItem>(result.call_id, result.result);
    }
    case FOUNDRY_LOCAL_ITEM_IMAGE: {
      const auto& image = static_cast<const ImageItem&>(item);
      if (image.data != nullptr && image.data_size > 0) {
        const auto* begin = static_cast<const uint8_t*>(image.data);
        auto clone = std::make_unique<ImageItem>(std::vector<uint8_t>(begin, begin + image.data_size), image.format);
        clone->uri = image.uri;
        return clone;
      }

      return std::make_unique<ImageItem>(image.uri, image.format);
    }
    case FOUNDRY_LOCAL_ITEM_AUDIO: {
      const auto& audio = static_cast<const AudioItem&>(item);
      if (audio.data == nullptr || audio.data_size == 0) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                 "request preflight audio input must contain bytes");
      }

      const auto* begin = static_cast<const uint8_t*>(audio.data);
      auto clone = std::make_unique<AudioItem>(
          std::vector<uint8_t>(begin, begin + audio.data_size), audio.format);
      clone->sample_rate = audio.sample_rate;
      clone->channels = audio.channels;
      return clone;
    }
    default:
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "request preflight does not support " + std::string(Item::TypeName(item.type)) + " items");
  }
}

}  // namespace

Request Request::CaptureChatSnapshot() const {
  Request snapshot;
  snapshot.options = options;
  snapshot.prepared_tool_definitions = prepared_tool_definitions;
  snapshot.forced_tool_choice = forced_tool_choice;
  snapshot.raw_envelope_descriptor = raw_envelope_descriptor;
  snapshot.item_segment_starts = item_segment_starts;
  snapshot.items.reserve(items.size());

  for (const auto* item : items) {
    if (item == nullptr) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "request preflight snapshot cannot contain a null item");
    }

    snapshot.AddOwnedItem(CloneChatItem(*item));
  }

  return snapshot;
}

}  // namespace fl
