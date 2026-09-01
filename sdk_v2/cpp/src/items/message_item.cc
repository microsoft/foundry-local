// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "items/message_item.h"

#include <cstdint>
#include <cstring>

#include "c_api_types.h"
#include "exception.h"
#include "items/audio_item.h"
#include "items/image_item.h"
#include "items/text_item.h"

namespace fl {

MessageItem::MessageItem(const MessageItem& other)
  : Item(other), role(other.role), tool_calls(other.tool_calls), name(other.name) {
  content.reserve(other.content.size());
  for (const auto& part : other.content) {
    if (!part.view) {
      continue;
    }
    content.push_back(MessagePart::Own(CloneApiPart(*part.view)));
  }
}

MessageItem& MessageItem::operator=(const MessageItem& other) {
  if (this == &other) {
    return *this;
  }

  Item::operator=(other);
  role = other.role;
  tool_calls = other.tool_calls;
  name = other.name;
  content.clear();
  content.reserve(other.content.size());
  for (const auto& part : other.content) {
    if (!part.view) {
      continue;
    }
    content.push_back(MessagePart::Own(CloneApiPart(*part.view)));
  }

  api_part_ptrs_.clear();
  return *this;
}

void MessageItem::SetMessageData(const flMessageData& new_data) {
  role = new_data.role;
  name = new_data.name ? new_data.name : "";

  content.clear();
  api_part_ptrs_.clear();

  if (new_data.content_items_count == 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "flMessageData requires at least one content_items entry");
  }
  if (!new_data.content_items) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "flMessageData.content_items is null but count > 0");
  }

  content.reserve(new_data.content_items_count);
  for (size_t i = 0; i < new_data.content_items_count; ++i) {
    const flItem* part = new_data.content_items[i];
    if (!part) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "flMessageData.content_items contains a null entry");
    }

    // flItem is an opaque ABI handle with no inheritance relationship to fl::Item; the runtime object is a
    // concrete fl::Item subclass and the bit pattern matches, so reinterpret_cast is the correct conversion.
    const Item* part_item = reinterpret_cast<const Item*>(part);
    switch (part_item->type) {
      case FOUNDRY_LOCAL_ITEM_TEXT:
      case FOUNDRY_LOCAL_ITEM_IMAGE:
      case FOUNDRY_LOCAL_ITEM_AUDIO:
        break;
      default:
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                 "flMessageData.content_items entry must be TEXT, IMAGE, or AUDIO");
    }

    // Deep-clone the part so the message owns it independently of the
    // caller's storage. Earlier versions kept borrowed views to save a copy,
    // but every realistic caller path (C++ wrapper temporaries, variadic
    // Request construction, language bindings) drops the source parts
    // immediately, leaving the message dangling. Cloning at the C ABI ingress
    // makes the contract symmetric with every other typed item.
    content.push_back(MessagePart::Own(CloneApiPart(*part_item)));
  }
}

void MessageItem::GetApiData(flMessageData& out) const {
  out.version = FOUNDRY_LOCAL_API_VERSION;
  out.role = role;
  out.name = name.empty() ? nullptr : name.c_str();

  api_part_ptrs_.clear();
  api_part_ptrs_.reserve(content.size());
  for (const auto& part : content) {
    api_part_ptrs_.push_back(part.view ? part.view->AsApiType() : nullptr);
  }

  out.content_items = api_part_ptrs_.empty() ? nullptr : api_part_ptrs_.data();
  out.content_items_count = api_part_ptrs_.size();
}

std::unique_ptr<Item> MessageItem::CloneApiPart(const Item& src) {
  switch (src.type) {
    case FOUNDRY_LOCAL_ITEM_TEXT: {
      const auto& t = static_cast<const TextItem&>(src);
      return std::make_unique<TextItem>(t.text, t.text_type);
    }
    case FOUNDRY_LOCAL_ITEM_IMAGE: {
      const auto& img = static_cast<const ImageItem&>(src);
      // Bytes-based: deep-copy into independently-owned storage so the clone
      // has no dependency on the source buffer's lifetime.
      if (img.data && img.data_size > 0) {
        const auto* bytes = static_cast<const std::uint8_t*>(img.data);
        std::vector<std::uint8_t> owned(bytes, bytes + img.data_size);
        auto clone = std::make_unique<ImageItem>(std::move(owned), img.format);
        clone->uri = img.uri;
        return clone;
      }
      // URI-only (or empty).
      auto clone = std::make_unique<ImageItem>(img.uri, img.format);
      return clone;
    }
    case FOUNDRY_LOCAL_ITEM_AUDIO: {
      const auto& aud = static_cast<const AudioItem&>(src);
      if (aud.data && aud.data_size > 0) {
        const auto* bytes = static_cast<const std::uint8_t*>(aud.data);
        std::vector<std::uint8_t> owned(bytes, bytes + aud.data_size);
        auto clone = std::make_unique<AudioItem>(std::move(owned), aud.format);
        clone->uri = aud.uri;
        clone->sample_rate = aud.sample_rate;
        clone->channels = aud.channels;
        return clone;
      }

      auto clone = std::make_unique<AudioItem>(aud.uri, aud.format);
      clone->sample_rate = aud.sample_rate;
      clone->channels = aud.channels;
      return clone;
    }
    default:
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported item type for message content part");
  }
}

}  // namespace fl
