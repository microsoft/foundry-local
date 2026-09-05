// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/chat_generator.h"

namespace fl {

std::optional<ChatTurnUsage> ChatGenerator::GetTurnUsage() const {
  return std::nullopt;
}

std::string ChatGenerator::GenerateAll() {
  std::string result;

  while (!IsDone()) {
    GenerateNextToken();
    result += Decode();
  }

  return result;
}

}  // namespace fl
