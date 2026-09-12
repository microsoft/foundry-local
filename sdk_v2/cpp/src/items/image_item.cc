// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "items/image_item.h"

#include "util/file_uri.h"

namespace fl {

std::vector<std::uint8_t> ImageItem::ReadBytes() const {
  if (data && data_size > 0) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    return std::vector<std::uint8_t>(p, p + data_size);
  }

  if (!uri.empty()) {
    return ReadFileUriBytes(uri, "image");
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
           "ImageItem must carry either bytes or a readable uri");
}

}  // namespace fl
