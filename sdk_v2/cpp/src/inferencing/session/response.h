// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <foundry_local/foundry_local_c.h>

#include "items/item.h"
#include "util/key_value_pairs.h"

namespace fl {

/// Token usage statistics for a generation request.
struct TokenUsage {
  int64_t prompt_tokens = 0;
  int64_t completion_tokens = 0;
  int64_t total_tokens = 0;
  int64_t reasoning_tokens = 0;
};

/// Generic inference response — in/out data container.
struct Response {
  std::vector<std::unique_ptr<Item>> items;
  flFinishReason finish_reason = FOUNDRY_LOCAL_FINISH_NONE;
  TokenUsage usage;
  // arbitrary response metadata (e.g. completion_id, created, model).
  // internal usage only currently but can be surfaced if needed.
  KeyValuePairs metadata;
};

}  // namespace fl
