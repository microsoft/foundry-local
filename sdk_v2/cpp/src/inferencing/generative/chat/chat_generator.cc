// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/chat_generator.h"

#include "exception.h"
#include "inferencing/generative/chat/prepared_chat_prompt.h"

namespace fl {

void ChatGenerator::Close() {}

int ChatGenerator::AppendPreparedPrompt(const std::vector<TranscriptMessage>&,
                                        const PreparedChatPrompt&,
                                        GenAIModelInstance&,
                                        const ToolCallContext&,
                                        const SearchOptions&) {
  FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "generator does not support prepared prompt append");
}

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
