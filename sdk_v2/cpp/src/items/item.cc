// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "items/item.h"

#include "c_api_types.h"
#include "items/audio_item.h"
#include "items/bytes_item.h"
#include "items/image_item.h"
#include "items/item_queue.h"
#include "items/message_item.h"
#include "items/tensor_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"
#include "exception.h"
#include "util/file_uri.h"

#include <cstdint>
namespace fl {
namespace {

std::unique_ptr<Item> CloneChatRequestItemImpl(const Item& item) {
  switch (item.type) {
    case FOUNDRY_LOCAL_ITEM_TEXT: {
      const auto& text = static_cast<const TextItem&>(item);
      return std::make_unique<TextItem>(text.text, text.text_type);
    }
    case FOUNDRY_LOCAL_ITEM_MESSAGE: {
      const auto& message = static_cast<const MessageItem&>(item);
      auto clone = std::make_unique<MessageItem>();
      clone->role = message.role;
      clone->name = message.name;
      clone->content.reserve(message.content.size());
      for (const auto& part : message.content) {
        if (part.view != nullptr) {
          clone->content.push_back(
              MessagePart::Own(CloneChatRequestItemImpl(*part.view)));
        }
      }

      return clone;
    }
    case FOUNDRY_LOCAL_ITEM_TOOL_CALL:
      return std::make_unique<ToolCallItem>(static_cast<const ToolCallItem&>(item));
    case FOUNDRY_LOCAL_ITEM_TOOL_RESULT:
      return std::make_unique<ToolResultItem>(static_cast<const ToolResultItem&>(item));
    case FOUNDRY_LOCAL_ITEM_IMAGE: {
      const auto& image = static_cast<const ImageItem&>(item);
      if (image.data != nullptr && image.data_size > 0) {
        const auto* bytes = static_cast<const std::uint8_t*>(image.data);
        auto clone = std::make_unique<ImageItem>(
            std::vector<std::uint8_t>(bytes, bytes + image.data_size), image.format);
        clone->uri = image.uri;
        return clone;
      }

      return std::make_unique<ImageItem>(image.uri, image.format);
    }
    case FOUNDRY_LOCAL_ITEM_AUDIO: {
      const auto& audio = static_cast<const AudioItem&>(item);
      std::unique_ptr<AudioItem> clone;
      if (audio.data != nullptr && audio.data_size > 0) {
        const auto* bytes = static_cast<const std::uint8_t*>(audio.data);
        clone = std::make_unique<AudioItem>(
            std::vector<std::uint8_t>(bytes, bytes + audio.data_size), audio.format);
        clone->uri = audio.uri;
      } else {
        clone = std::make_unique<AudioItem>(audio.uri, audio.format);
      }

      clone->sample_rate = audio.sample_rate;
      clone->channels = audio.channels;
      return clone;
    }
    case FOUNDRY_LOCAL_ITEM_QUEUE:
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "ItemQueue cannot be captured for request preflight");
    default:
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "unsupported item type for chat request preflight snapshot: " +
                   std::string(Item::TypeName(item.type)));
  }
}

}  // namespace

std::vector<std::uint8_t> AudioItem::ReadBytes() const {
  if (data != nullptr && data_size > 0) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    return {bytes, bytes + data_size};
  }

  if (!uri.empty()) {
    return ReadFileUriBytes(uri, "audio");
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "AudioItem must carry either bytes or a readable uri");
}

// flItem / flItemQueue are opaque ABI handle types — they are NOT base classes of fl::Item / fl::ItemQueue.
// The bit pattern of `this` is what the C ABI sees, but the C++ static type system has no relationship
// between them, so we must reinterpret_cast (not static_cast) to get a flItem*. See c_api_types.h.
flItem* Item::AsApiType() noexcept { return reinterpret_cast<flItem*>(this); }

const flItem* Item::AsApiType() const noexcept { return reinterpret_cast<const flItem*>(this); }

flItemQueue* ItemQueue::AsApiType() noexcept { return reinterpret_cast<flItemQueue*>(this); }

// Used from C API. Internal usage should create the type directly with the relevant data for the item.
std::unique_ptr<Item> Item::Create(flItemType type) {
  switch (type) {
    case FOUNDRY_LOCAL_ITEM_TEXT:
      return std::make_unique<TextItem>();
    case FOUNDRY_LOCAL_ITEM_MESSAGE:
      return std::make_unique<MessageItem>();
    case FOUNDRY_LOCAL_ITEM_TOOL_CALL:
      return std::make_unique<ToolCallItem>();
    case FOUNDRY_LOCAL_ITEM_TOOL_RESULT:
      return std::make_unique<ToolResultItem>();
    case FOUNDRY_LOCAL_ITEM_IMAGE:
      return std::make_unique<ImageItem>();
    case FOUNDRY_LOCAL_ITEM_AUDIO:
      return std::make_unique<AudioItem>();
    case FOUNDRY_LOCAL_ITEM_TENSOR:
      return std::make_unique<TensorItem>();
    case FOUNDRY_LOCAL_ITEM_BYTES:
      return std::make_unique<BytesItem>();
    case FOUNDRY_LOCAL_ITEM_QUEUE:
      return std::make_unique<ItemQueue>();
    default:
      return nullptr;
  }
}

std::string_view Item::TypeName(flItemType type) noexcept {
  switch (type) {
    case FOUNDRY_LOCAL_ITEM_UNKNOWN:
      return "UNKNOWN";
    case FOUNDRY_LOCAL_ITEM_BYTES:
      return "BYTES";
    case FOUNDRY_LOCAL_ITEM_TENSOR:
      return "TENSOR";
    case FOUNDRY_LOCAL_ITEM_TEXT:
      return "TEXT";
    case FOUNDRY_LOCAL_ITEM_MESSAGE:
      return "MESSAGE";
    case FOUNDRY_LOCAL_ITEM_IMAGE:
      return "IMAGE";
    case FOUNDRY_LOCAL_ITEM_AUDIO:
      return "AUDIO";
    case FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT:
      return "SPEECH_SEGMENT";
    case FOUNDRY_LOCAL_ITEM_SPEECH_RESULT:
      return "SPEECH_RESULT";
    case FOUNDRY_LOCAL_ITEM_TOOL_CALL:
      return "TOOL_CALL";
    case FOUNDRY_LOCAL_ITEM_TOOL_RESULT:
      return "TOOL_RESULT";
    case FOUNDRY_LOCAL_ITEM_QUEUE:
      return "QUEUE";
    default:
      return "UNKNOWN";
  }
}

std::unique_ptr<Item> CloneChatRequestItem(const Item& item) {
  return CloneChatRequestItemImpl(item);
}

}  // namespace fl
