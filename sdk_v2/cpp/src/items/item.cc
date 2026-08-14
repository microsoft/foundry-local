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

namespace fl {

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

}  // namespace fl
