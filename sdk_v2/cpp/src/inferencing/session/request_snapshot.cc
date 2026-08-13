// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/request_snapshot.h"

#include "items/message_item.h"
#include "items/text_item.h"
#include "items/tool_call_item.h"
#include "items/tool_result_item.h"

#include <utility>

namespace fl {

namespace {

/// Deep-clone a *borrowed* item so the snapshot owns it outright.
///
/// Only the self-contained value types are clonable. Everything else — a live ItemQueue, a TensorItem whose
/// buffer belongs to a custom deleter, a BytesItem/AudioItem/ImageItem pointing at caller memory — either
/// cannot be copied at all or would silently change meaning if it were, so those stay borrowed and the
/// AddBorrowedItem contract ("the caller keeps it alive") continues to apply for the duration of the run.
std::shared_ptr<Item> CloneBorrowedItem(const Item& source) {
  switch (source.type) {
    case FOUNDRY_LOCAL_ITEM_TEXT: {
      const auto& text = static_cast<const TextItem&>(source);
      return std::make_shared<TextItem>(text.text, text.text_type);
    }

    case FOUNDRY_LOCAL_ITEM_MESSAGE:
      // MessageItem's copy constructor deep-clones every part (see MessageItem::CloneApiPart), including
      // image/audio payloads, so the clone has no dependency on the source's lifetime.
      return std::make_shared<MessageItem>(static_cast<const MessageItem&>(source));

    case FOUNDRY_LOCAL_ITEM_TOOL_RESULT: {
      const auto& result = static_cast<const ToolResultItem&>(source);
      return std::make_shared<ToolResultItem>(result.call_id, result.result);
    }

    case FOUNDRY_LOCAL_ITEM_TOOL_CALL: {
      const auto& call = static_cast<const ToolCallItem&>(source);
      return std::make_shared<ToolCallItem>(call.call_id, call.name, call.arguments);
    }

    default:
      return nullptr;
  }
}

}  // namespace

std::shared_ptr<const RequestSnapshot> RequestSnapshot::Capture(const Request& source) {
  // make_shared cannot reach the private constructor; the snapshot is built here and handed out as const.
  std::shared_ptr<RequestSnapshot> snapshot(new RequestSnapshot());

  snapshot->data_.options = source.options;
  snapshot->data_.SetTimeout(source.Timeout());
  snapshot->data_.items.reserve(source.items.size());

  for (Item* item : source.items) {
    if (item == nullptr) {
      continue;
    }

    // Exact shared ownership for anything the Request already owns: no copy, identity preserved, and the
    // caller can drop the Request without freeing it.
    if (auto retained = source.FindOwnedItem(item)) {
      snapshot->data_.AddRetainedItem(std::move(retained));
      continue;
    }

    if (auto cloned = CloneBorrowedItem(*item)) {
      snapshot->data_.AddRetainedItem(std::move(cloned));
      continue;
    }

    snapshot->data_.AddBorrowedItem(item);
    snapshot->has_borrowed_items_ = true;
  }

  return snapshot;
}

}  // namespace fl
