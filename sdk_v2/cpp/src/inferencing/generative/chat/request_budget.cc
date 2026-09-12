// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/request_budget.h"

#include "exception.h"

#include <limits>

namespace fl {

RequestBudget ComputeRequestBudget(int64_t prompt_tokens,
                                   int64_t output_reserve_tokens,
                                   int64_t context_limit_tokens) {
  if (prompt_tokens < 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "prompt token count must not be negative");
  }
  if (output_reserve_tokens < 1) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "max_output_tokens must be >= 1");
  }
  if (context_limit_tokens < 1) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "model context length must be a positive integer");
  }
  if (prompt_tokens > (std::numeric_limits<int64_t>::max)() - output_reserve_tokens) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "request token budget overflow");
  }

  const int64_t required_tokens = prompt_tokens + output_reserve_tokens;
  const bool fits = required_tokens <= context_limit_tokens;
  return RequestBudget{
      .prompt_tokens = prompt_tokens,
      .output_reserve_tokens = output_reserve_tokens,
      .required_tokens = required_tokens,
      .context_limit_tokens = context_limit_tokens,
      .fits = fits,
      .deficit_tokens = fits ? 0 : required_tokens - context_limit_tokens,
  };
}

}  // namespace fl
