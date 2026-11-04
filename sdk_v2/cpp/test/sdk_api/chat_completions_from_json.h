// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Forward declarations for response/streaming from_json — test use only.
// These are implemented in chat_completions_from_json.cc and intentionally
// excluded from production code (which only serializes responses via to_json).
#pragma once

#include "chat_completions.h"

namespace fl {

// --- Response deserialization ---
void from_json(const nlohmann::json& j, PromptTokensDetails& d);
void from_json(const nlohmann::json& j, CompletionTokensDetails& d);
void from_json(const nlohmann::json& j, ChatCompletionUsage& u);
void from_json(const nlohmann::json& j, ChatCompletionFunctionCall& f);
void from_json(const nlohmann::json& j, ChatCompletionCustomCall& c);
void from_json(const nlohmann::json& j, ChatCompletionToolCall& tc);
void from_json(const nlohmann::json& j, ChatCompletionResponseMessage& m);
void from_json(const nlohmann::json& j, ChatCompletionChoice& c);
void from_json(const nlohmann::json& j, ChatCompletionResponse& r);

// --- Streaming deserialization ---
void from_json(const nlohmann::json& j, ChatCompletionDelta& d);
void from_json(const nlohmann::json& j, ChatCompletionChunkChoice& c);
void from_json(const nlohmann::json& j, ChatCompletionChunk& c);

}  // namespace fl
