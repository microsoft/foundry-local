// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "items/item.h"

#include <cstdint>
#include <vector>

namespace fl {

/// Tensor data with shape and element type.
struct TensorItem : Item {
  flTensorDataType data_type = FOUNDRY_LOCAL_TENSOR_UNDEFINED;
  const void* data = nullptr;
  void* mutable_data = nullptr;
  std::vector<int64_t> shape;
  flTensorDataDeleter deleter_ = nullptr;
  void* deleter_user_data_ = nullptr;

  TensorItem(flTensorDataType data_type_in = FOUNDRY_LOCAL_TENSOR_UNDEFINED, const void* data_in = nullptr,
             std::vector<int64_t> shape_in = {})
      : Item(FOUNDRY_LOCAL_ITEM_TENSOR),
        data_type(data_type_in),
        data(data_in),
        shape(std::move(shape_in)) {}

  ~TensorItem() override {
    if (deleter_) {
      flTensorData td{};
      GetApiData(td);
      td.mutable_data = mutable_data;
      deleter_(&td, deleter_user_data_);
    }
  }

  TensorItem(const TensorItem& other) = delete;
  TensorItem& operator=(const TensorItem& other) = delete;

  void SetTensorData(const flTensorData& new_data) {
    data_type = new_data.data_type;
    data = new_data.data;
    mutable_data = new_data.mutable_data;
    shape.assign(new_data.shape, new_data.shape + new_data.rank);

    if (!data && mutable_data) {
      data = mutable_data;
    }

    deleter_ = new_data.deleter;
    deleter_user_data_ = new_data.deleter_user_data;
  }

  void GetApiData(flTensorData& out) const {
    out.version = FOUNDRY_LOCAL_DATA_VERSION;
    out.data_type = data_type;
    out.data = data;
    out.mutable_data = nullptr;
    out.shape = shape.data();
    out.rank = shape.size();
    out.deleter = nullptr;
    out.deleter_user_data = nullptr;
  }
};

}  // namespace fl
