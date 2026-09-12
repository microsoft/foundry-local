// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "items/item.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fl {

/// Audio data — either raw bytes (data + data_size + format) or a URI.
struct AudioItem : Item {
  const void* data = nullptr;
  void* mutable_data = nullptr;
  size_t data_size = 0;
  std::string format;
  std::string uri;
  int sample_rate = 0;
  int channels = 0;
  flAudioDataDeleter deleter_ = nullptr;
  void* deleter_user_data_ = nullptr;

  AudioItem(const void* data_in = nullptr, size_t data_size_in = 0, std::string format_in = {})
      : Item(FOUNDRY_LOCAL_ITEM_AUDIO),
        data(data_in),
        data_size(data_size_in),
        format(std::move(format_in)) {}

  AudioItem(std::string uri_in, std::string format_in = {})
      : Item(FOUNDRY_LOCAL_ITEM_AUDIO),
        data(nullptr),
        data_size(0),
        format(std::move(format_in)),
        uri(std::move(uri_in)) {}

  /// Construct an AudioItem that owns its bytes by holding a vector. The
  /// `data` pointer is set to the vector's storage and remains stable for the
  /// item's lifetime (the vector is not resized after construction).
  AudioItem(std::vector<std::uint8_t> owned_bytes, std::string format_in = {})
      : Item(FOUNDRY_LOCAL_ITEM_AUDIO),
        data_size(owned_bytes.size()),
        format(std::move(format_in)),
        owned_data_(std::move(owned_bytes)) {
    data = owned_data_.empty() ? nullptr : owned_data_.data();
  }

  ~AudioItem() override {
    if (deleter_) {
      flAudioData ad{};
      GetApiData(ad);
      ad.mutable_data = mutable_data;
      deleter_(&ad, deleter_user_data_);
    }
  }

  AudioItem(const AudioItem& other) = delete;
  AudioItem& operator=(const AudioItem& other) = delete;

  void SetAudioData(const flAudioData& new_data) {
    data = new_data.data;
    mutable_data = new_data.mutable_data;
    data_size = new_data.data_size;
    format = new_data.format ? new_data.format : "";
    uri = new_data.uri ? new_data.uri : "";
    sample_rate = new_data.sample_rate;
    channels = new_data.channels;

    if (!data && mutable_data) {
      data = mutable_data;
    }

    deleter_ = new_data.deleter;
    deleter_user_data_ = new_data.deleter_user_data;
  }

  void GetApiData(flAudioData& out) const {
    out.version = FOUNDRY_LOCAL_API_VERSION;
    out.data = data;
    out.mutable_data = nullptr;
    out.data_size = data_size;
    out.format = format.empty() ? nullptr : format.c_str();
    out.uri = uri.empty() ? nullptr : uri.c_str();
    out.sample_rate = sample_rate;
    out.channels = channels;
    out.deleter = nullptr;
    out.deleter_user_data = nullptr;
  }

  /// Return owned or borrowed bytes, reading a URI only when no bytes are present.
  std::vector<std::uint8_t> ReadBytes() const;

 private:
  // Optional owning storage when the item was constructed with a vector of
  // bytes. The `data` pointer is set to point into this buffer at construction.
  std::vector<std::uint8_t> owned_data_;
};

}  // namespace fl
