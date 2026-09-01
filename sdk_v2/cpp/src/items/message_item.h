// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "items/item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "exception.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fl {

/// A single content part of a MessageItem. Either:
///   - borrowed: `view` points at an Item owned elsewhere, `owned` is null.
///   - owned:    `owned` holds the Item, `view == owned.get()`.
/// In both cases `view` is the canonical accessor and is non-null after
/// construction.
struct MessagePart {
  const Item* view = nullptr;
  std::unique_ptr<Item> owned;

  // Borrowed view: caller guarantees the source outlives this MessagePart
  // (or this MessagePart is upgraded to owned via deep-clone before the
  // source goes away).
  static MessagePart Borrow(const Item& src) noexcept {
    MessagePart p;
    p.view = &src;
    return p;
  }

  // Owning: takes ownership of the Item.
  static MessagePart Own(std::unique_ptr<Item> item) noexcept {
    MessagePart p;
    p.view = item.get();
    p.owned = std::move(item);
    return p;
  }

  bool IsOwned() const noexcept { return owned != nullptr; }
};

/// Chat message with role, typed content parts, and optional participant name.
///
/// `content` is a list of part items (TextItem / ImageItem / AudioItem). Other
/// item types are rejected at the C ABI boundary. Most messages are plain text
/// and use the single-string convenience constructor; multi-modal callers
/// (e.g. vision input) construct with an explicit parts vector.
///
/// Lifetime model:
///   - Parts may be either borrowed (view-only, no allocation) or owned.
///   - SetMessageData (the C ABI ingress) records borrowed views — the C
///     caller's part flItem*s only need to live for the duration of that
///     SetMessage call IF the resulting MessageItem is not retained. Anything
///     that retains a MessageItem (e.g. ChatSession history) MUST copy it.
///   - The copy constructor / copy assignment deep-clone every part into
///     independently-owned storage, including duplicating any IMAGE/AUDIO
///     byte buffers. So a copied MessageItem is fully self-contained.
///   - Move construction / move assignment transfer parts as-is (no clone).
struct MessageItem : Item {
  flMessageRole role;
  std::vector<MessagePart> content;
  std::vector<ToolCallItem> tool_calls;
  std::string name;

  // C API usage
  MessageItem() : Item(FOUNDRY_LOCAL_ITEM_MESSAGE), role{FOUNDRY_LOCAL_ROLE_NONE} {}

  // Single-text convenience: wraps `text` in an owned TextItem part. Throws
  // FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT if `text` is empty.
  MessageItem(flMessageRole role_in, std::string text, std::string name_in = {})
      : Item(FOUNDRY_LOCAL_ITEM_MESSAGE),
        role(role_in),
        name(std::move(name_in)) {
    if (text.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "MessageItem requires non-empty text");
    }

    content.push_back(MessagePart::Own(std::make_unique<TextItem>(std::move(text))));
  }

  // Multi-part: caller transfers ownership of part items (TextItem/ImageItem/AudioItem).
  // Throws FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT if `parts` is empty or contains a null entry.
  MessageItem(flMessageRole role_in,
              std::vector<std::unique_ptr<Item>> parts,
              std::string name_in = {})
      : Item(FOUNDRY_LOCAL_ITEM_MESSAGE),
        role(role_in),
        name(std::move(name_in)) {
    if (parts.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "MessageItem requires at least one content part");
    }

    content.reserve(parts.size());
    for (auto& p : parts) {
      if (!p) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "MessageItem content part must not be null");
      }

      content.push_back(MessagePart::Own(std::move(p)));
    }
  }

  MessageItem(const MessageItem& other);
  MessageItem& operator=(const MessageItem& other);
  MessageItem(MessageItem&&) = default;
  MessageItem& operator=(MessageItem&&) = default;

  /// True when the message has exactly one part and it is a TextItem.
  bool IsSimpleText() const {
    return content.size() == 1 && content.front().view &&
           content.front().view->type == FOUNDRY_LOCAL_ITEM_TEXT;
  }

  /// Text of the single TextItem part. Throws if !IsSimpleText().
  std::string GetSimpleText() const {
    if (!IsSimpleText()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "MessageItem is not a single TextItem");
    }
    return static_cast<const TextItem&>(*content.front().view).text;
  }

  void SetMessageData(const flMessageData& new_data);
  void GetApiData(flMessageData& out) const;

 private:
  // Storage for pointers returned by GetApiData. Mutable so a const message
  // can serve API-level reads.
  mutable std::vector<const flItem*> api_part_ptrs_;

  // Clone a part item into a self-owned unique_ptr. For IMAGE/AUDIO parts,
  // byte buffers are deep-copied into independently-owned storage so the
  // clone has no dependency on the source's lifetime. Implementation lives
  // out-of-line (see message_item.cc) to avoid pulling every concrete item
  // header into this one.
  static std::unique_ptr<Item> CloneApiPart(const Item& src);
};

}  // namespace fl
