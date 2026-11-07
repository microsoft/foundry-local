// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "items/item.h"

#include <cstddef>

namespace fl {

/// Raw bytes tagged with the item type they represent.
struct BytesItem : Item {
  flItemType item_type = FOUNDRY_LOCAL_ITEM_UNKNOWN;
  const void* data = nullptr;
  void* mutable_data = nullptr;
  size_t data_size = 0;
  flBytesDataDeleter deleter_ = nullptr;
  void* deleter_user_data_ = nullptr;

  BytesItem(flItemType item_type_in = FOUNDRY_LOCAL_ITEM_UNKNOWN, const void* data_in = nullptr,
            size_t data_size_in = 0)
      : Item(FOUNDRY_LOCAL_ITEM_BYTES),
        item_type(item_type_in),
        data(data_in),
        data_size(data_size_in) {}

  ~BytesItem() override {
    if (deleter_) {
      flBytesData bd{};
      GetApiData(bd);
      bd.mutable_data = mutable_data;
      deleter_(&bd, deleter_user_data_);
    }
  }

  BytesItem(const BytesItem& other) = delete;
  BytesItem& operator=(const BytesItem& other) = delete;

  void SetBytesData(const flBytesData& new_data) {
    item_type = new_data.item_type;
    data = new_data.data;
    mutable_data = new_data.mutable_data;
    data_size = new_data.data_size;

    if (!data && mutable_data) {
      data = mutable_data;
    }

    deleter_ = new_data.deleter;
    deleter_user_data_ = new_data.deleter_user_data;
  }

  void GetApiData(flBytesData& out) const {
    out.version = FOUNDRY_LOCAL_DATA_VERSION;
    out.item_type = item_type;
    out.data = data;
    out.mutable_data = nullptr;
    out.data_size = data_size;
    out.deleter = nullptr;
    out.deleter_user_data = nullptr;
  }
};

}  // namespace fl
