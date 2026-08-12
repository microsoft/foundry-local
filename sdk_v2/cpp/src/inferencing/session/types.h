// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <string>
#include <string_view>

namespace fl {

/// The kind of inference a session performs.
enum class SessionType {
  kChat,
  kAudio,
  kPredictive,
  kEmbeddings,
};

constexpr bool IsTextGenerationTask(std::string_view task) noexcept {
  return task == "chat-completion" || task == "text-generation" || task == "text2text-generation";
}

struct ToolDefinition {
  std::string name;
  std::string description;
  std::string json_schema;
};

}  // namespace fl
