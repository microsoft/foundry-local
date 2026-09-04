// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <foundry_local/foundry_local_c.h>

#include <memory>
#include <string_view>

#include "util/key_value_pairs.h"

namespace fl {

/// Base class for all item types in a Request / Response.
/// The `type` discriminator identifies the concrete derived type.
/// Use Item::Create to construct the correct derived type for a given flItemType.
struct Item {
  virtual ~Item() = default;

  // Item types that are self-contained (no potential external data to manage) can be copied.
  // metadata is not copied: copied items are internal-only and don't own external data.
  // We explicitly enable copying for derived types that allow it.
  // metadata _could_ be copied if there's a use-case that requires it.
  Item(const Item& other) : type(other.type) {}
  Item& operator=(const Item& other) {
    if (this != &other) {
      type = other.type;
    }
    return *this;
  }

  Item(Item&&) = default;
  Item& operator=(Item&&) = default;

  flItemType type;

  const KeyValuePairs* GetMetadata() const {
    return metadata.get();
  }

  KeyValuePairs& GetMetadata() {
    if (!metadata) {
      metadata = std::make_unique<KeyValuePairs>();
    }

    return *metadata;
  }

  flItem* AsApiType() noexcept;
  const flItem* AsApiType() const noexcept;

  /// Factory: creates the correct derived type for the given flItemType.
  static std::unique_ptr<Item> Create(flItemType type);

  /// Return the stable symbolic name for an item type.
  static std::string_view TypeName(flItemType type) noexcept;

 protected:
  explicit Item(flItemType type)
      : type(type) {}

 private:
  /// Lazily-created key-value metadata. Any item can carry arbitrary metadata.
  mutable std::unique_ptr<KeyValuePairs> metadata;
};

}  // namespace fl
