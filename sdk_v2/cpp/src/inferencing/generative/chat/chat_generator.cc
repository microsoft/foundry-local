// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/chat_generator.h"

#include "exception.h"

namespace fl {

void ChatGenerator::RewindTo(int /*token_count*/) {
  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "This generator does not support rewinding retained model state");
}

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
