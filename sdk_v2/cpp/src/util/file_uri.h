// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "exception.h"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

/// Convert a path that may be a `file://` URI into a plain filesystem path.
/// Strips the optional `file://` scheme prefix and percent-decodes any
/// %XX escape sequences. Inputs without the scheme are returned unchanged
/// except for percent-decoding, so plain paths pass through cleanly.
inline std::string PathFromFileUri(std::string_view uri) {
  constexpr std::string_view kFileScheme = "file://";
  if (uri.size() >= kFileScheme.size() &&
      uri.compare(0, kFileScheme.size(), kFileScheme) == 0) {
    uri.remove_prefix(kFileScheme.size());
  }

  std::string out;
  out.reserve(uri.size());

  for (size_t i = 0; i < uri.size(); ++i) {
    if (uri[i] == '%' && i + 2 < uri.size() &&
        std::isxdigit(static_cast<unsigned char>(uri[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(uri[i + 2]))) {
      auto hex = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
      };
      out.push_back(static_cast<char>((hex(uri[i + 1]) << 4) | hex(uri[i + 2])));
      i += 2;
    } else {
      out.push_back(uri[i]);
    }
  }

  return out;
}

inline std::vector<std::uint8_t> ReadFileUriBytes(std::string_view uri, std::string_view media_kind) {
  const auto path = PathFromFileUri(uri);
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "failed to open ", media_kind, " file: ", path);
  }

  const auto size = input.tellg();
  if (size <= 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, media_kind, " file is empty: ", path);
  }

  input.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (!input.read(reinterpret_cast<char*>(bytes.data()), size)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "failed to read ", media_kind, " file: ", path);
  }

  return bytes;
}

}  // namespace fl
