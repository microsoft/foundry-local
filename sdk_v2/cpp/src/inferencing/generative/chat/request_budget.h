// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstdint>

namespace fl {

struct RequestBudget {
  int64_t prompt_tokens = 0;
  int64_t output_reserve_tokens = 0;
  int64_t required_tokens = 0;
  int64_t context_limit_tokens = 0;
  bool fits = false;
  int64_t deficit_tokens = 0;
};

/// Compute a request budget with checked signed arithmetic.
RequestBudget ComputeRequestBudget(int64_t prompt_tokens,
                                   int64_t output_reserve_tokens,
                                   int64_t context_limit_tokens);

}  // namespace fl
