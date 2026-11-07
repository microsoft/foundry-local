// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace fl {

struct HarmonyProtocol {
  static constexpr const char* kAssistantPrefix = "<|start|>assistant";
  int32_t start;
  int32_t channel;
  int32_t message;
  int32_t end;
  int32_t handoff;
  int32_t answer;
  int32_t format;
  std::vector<int32_t> assistant_prefix_tokens;
};

struct HarmonyContinuation {
  std::string header;
  size_t header_tokens = 0;
  size_t header_bytes = 0;
};

}  // namespace fl
