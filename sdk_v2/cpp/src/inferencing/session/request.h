// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/types.h"
#include "items/item.h"
#include "util/key_value_pairs.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fl {

/// Well-known request option: a leading system message applied to this turn's prompt without becoming conversation
/// history.
///
/// Chat sessions use it for request-scoped system instructions (the Responses API's `instructions`). Keeping it out
/// of the message list is the point: it can neither accumulate a copy per turn nor be replayed from a stored
/// conversation, and the value the current request supplies always wins.
inline constexpr const char* kSystemPromptOption = "system_prompt";

/// Generic inference request — pure input data.
/// Items are stored as borrowed pointers. Owned items are kept alive in owned_items.
struct Request {
  std::vector<Item*> items;  // all items (borrowed pointers)
  KeyValuePairs options;
  /// Request-local definitions already validated by an HTTP adapter. Used only to carry Chat
  /// declarations across the asynchronous streaming boundary without parsing or registering twice.
  std::optional<std::vector<ToolDefinition>> prepared_tool_definitions;

  /// Start indices, into `items`, of the replay segments the producer knows about. Ascending, and empty means the
  /// whole list is one segment.
  ///
  /// A producer that reconstructs a stored conversation marks where each recorded turn's items begin. Consumers
  /// must not group items across a boundary, so two recorded turns can never collapse into one message. A producer
  /// with no boundary information — a caller resending a flat conversation — leaves this empty and gets
  /// adjacency-based grouping instead.
  ///
  /// An index may repeat or sit past the end of `items`: a recorded turn that contributed no items still separates
  /// the turns around it. Use BeginItemSegment rather than writing indices directly — it is what keeps them
  /// ascending and consistent with `items`.
  std::vector<size_t> item_segment_starts;

  /// Cancellation flag — set by the C API or streaming callback handler to cancel
  /// an in-flight request. Checked in generation loops. Atomic because it is written
  /// by one thread (callback worker or C API) and read by another (generator loop).
  /// Uses relaxed ordering since it is a one-way flag and exact timing doesn't matter.
  mutable std::atomic<bool> canceled{false};

  Request() = default;

  Request(Request&& other) noexcept
      : items(std::move(other.items)),
        options(std::move(other.options)),
        prepared_tool_definitions(std::move(other.prepared_tool_definitions)),
        item_segment_starts(std::move(other.item_segment_starts)),
        canceled(other.canceled.load(std::memory_order_relaxed)),
        owned_items(std::move(other.owned_items)) {}

  Request& operator=(Request&& other) noexcept {
    items = std::move(other.items);
    options = std::move(other.options);
    prepared_tool_definitions = std::move(other.prepared_tool_definitions);
    item_segment_starts = std::move(other.item_segment_starts);
    canceled.store(other.canceled.load(std::memory_order_relaxed), std::memory_order_relaxed);
    owned_items = std::move(other.owned_items);
    return *this;
  }

  Request(const Request&) = delete;
  Request& operator=(const Request&) = delete;

  /// Add a pre-allocated owned item.
  void AddOwnedItem(std::unique_ptr<Item> item) {
    items.push_back(item.get());
    owned_items.push_back(std::move(item));
  }

  /// Add a borrowed item (caller must keep it alive).
  void AddBorrowedItem(Item* item) {
    items.push_back(item);
  }

  /// Mark the next item added as the first of a new replay segment. Recording a segment that then adds no items is
  /// meaningful: the boundary still keeps the turns on either side of it apart.
  void BeginItemSegment() {
    item_segment_starts.push_back(items.size());
  }

 private:
  std::vector<std::unique_ptr<Item>> owned_items;  // owned items (lifetime)
};

}  // namespace fl
