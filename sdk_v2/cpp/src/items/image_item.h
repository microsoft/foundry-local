// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "items/item.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fl {

/// Image data — either raw bytes (data + data_size + format) or a URI.
struct ImageItem : Item {
  const void* data = nullptr;
  void* mutable_data = nullptr;
  size_t data_size = 0;
  std::string format;
  std::string uri;
  flImageDataDeleter deleter_ = nullptr;
  void* deleter_user_data_ = nullptr;

  ImageItem(const void* data_in = nullptr, size_t data_size_in = 0, std::string format_in = {})
      : Item(FOUNDRY_LOCAL_ITEM_IMAGE),
        data(data_in),
        data_size(data_size_in),
        format(std::move(format_in)) {}

  ImageItem(std::string uri, std::string format = {})
      : Item(FOUNDRY_LOCAL_ITEM_IMAGE), data(nullptr), data_size(0), format(std::move(format)), uri(std::move(uri)) {}

  /// Construct an ImageItem that owns its bytes by holding a vector. The
  /// `data` pointer is set to the vector's storage and remains stable for the
  /// item's lifetime (the vector is not resized after construction).
  ImageItem(std::vector<std::uint8_t> owned_bytes, std::string format_in = {})
      : Item(FOUNDRY_LOCAL_ITEM_IMAGE),
        data_size(owned_bytes.size()),
        format(std::move(format_in)),
        owned_data_(std::move(owned_bytes)) {
    data = owned_data_.empty() ? nullptr : owned_data_.data();
  }

  ~ImageItem() override {
    if (deleter_) {
      flImageData id{};
      GetApiData(id);
      id.mutable_data = mutable_data;
      deleter_(&id, deleter_user_data_);
    }
  }

  ImageItem(const ImageItem& other) = delete;
  ImageItem& operator=(const ImageItem& other) = delete;

  void SetImageData(const flImageData& new_data) {
    data = new_data.data;
    mutable_data = new_data.mutable_data;
    data_size = new_data.data_size;
    format = new_data.format ? new_data.format : "";
    uri = new_data.uri ? new_data.uri : "";

    if (!data && mutable_data) {
      data = mutable_data;
    }

    deleter_ = new_data.deleter;
    deleter_user_data_ = new_data.deleter_user_data;
  }

  void GetApiData(flImageData& out) const {
    out.version = FOUNDRY_LOCAL_DATA_VERSION;
    out.data = data;
    out.mutable_data = nullptr;
    out.data_size = data_size;
    out.format = format.empty() ? nullptr : format.c_str();
    out.uri = uri.empty() ? nullptr : uri.c_str();
    out.deleter = nullptr;
    out.deleter_user_data = nullptr;
  }

  /// Return a copy of the image's bytes. Reads from the in-memory buffer
  /// when present (`data` + `data_size`); otherwise treats `uri` as a local
  /// file path (the optional `file://` prefix is stripped).
  ///
  /// Throws FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT when the item carries no
  /// bytes and no readable URI.
  std::vector<std::uint8_t> ReadBytes() const;

 private:
  // Optional owning storage when the item was constructed with a vector of
  // bytes. The `data` pointer is set to point into this buffer at construction.
  std::vector<std::uint8_t> owned_data_;
};

}  // namespace fl
