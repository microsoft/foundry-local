// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "items/image_item.h"

#include "exception.h"
#include "util/file_uri.h"

#include <azure/core/base64.hpp>

#include <fstream>
#include <ios>
#include <string>
#include <string_view>

namespace fl {

std::unique_ptr<ImageItem> ImageItem::FromDataUrl(const std::string& url, std::string_view error_source) {
  if (!url.starts_with("data:")) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, std::string(error_source) + " must start with 'data:'");
  }

  const auto comma = url.find(',');
  if (comma == std::string::npos) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, std::string(error_source) + " is missing payload separator ','");
  }

  const std::string header = url.substr(5, comma - 5);
  constexpr std::string_view kBase64Marker = ";base64";
  const auto base64_pos = header.find(kBase64Marker);
  if (base64_pos == std::string::npos) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, std::string(error_source) + " must use ';base64,' encoding");
  }

  std::vector<std::uint8_t> bytes;
  try {
    bytes = Azure::Core::Convert::Base64Decode(url.substr(comma + 1));
  } catch (const std::exception& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             std::string(error_source) + " has invalid base64 payload: " + e.what());
  }

  if (bytes.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, std::string(error_source) + " decoded to zero bytes");
  }

  return std::make_unique<ImageItem>(std::move(bytes), header.substr(0, base64_pos));
}

std::vector<std::uint8_t> ImageItem::ReadBytes() const {
  if (data && data_size > 0) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    return std::vector<std::uint8_t>(p, p + data_size);
  }

  if (!uri.empty()) {
    // Strip optional `file://` scheme prefix and percent-decode so URIs like
    // `file:///C:/My%20Image.png` work the same as a plain path.
    std::string path = PathFromFileUri(uri);

    std::ifstream in(path, std::ios::binary);
    if (!in) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               std::string("failed to open image file: ") + path);
    }

    in.seekg(0, std::ios::end);
    auto size = in.tellg();
    in.seekg(0, std::ios::beg);

    if (size <= 0) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               std::string("image file is empty: ") + path);
    }

    std::vector<std::uint8_t> bytes(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               std::string("failed to read image file: ") + path);
    }

    return bytes;
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
           "ImageItem must carry either bytes or a readable uri");
}

}  // namespace fl
