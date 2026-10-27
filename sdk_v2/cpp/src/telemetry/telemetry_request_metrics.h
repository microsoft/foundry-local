// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>

namespace fl::TelemetryInternal {

inline std::optional<uint64_t> CountParsedJsonMessages(const nlohmann::json& request) {
  const auto messages = request.find("messages");
  if (messages == request.end()) {
    return uint64_t{0};
  }
  if (!messages->is_array()) {
    return std::nullopt;
  }
  return messages->size();
}

}  // namespace fl::TelemetryInternal
