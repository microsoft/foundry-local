// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "contracts/reasoning_options.h"

#include "exception.h"

#include <array>
#include <string_view>

namespace fl {

nlohmann::json ResolveReasoningTemplateKwargs(
    std::optional<nlohmann::json> template_kwargs,
    const std::optional<std::string>& reasoning_effort) {
  nlohmann::json kwargs = template_kwargs.value_or(nlohmann::json::object());
  if (!kwargs.is_object()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "chat_template_kwargs must be a valid JSON object");
  }

  if (!reasoning_effort.has_value()) {
    return kwargs;
  }

  constexpr std::array<std::string_view, 6> kSupportedEfforts = {
      "none", "minimal", "low", "medium", "high", "xhigh"};
  const std::string_view effort = *reasoning_effort;
  bool supported = false;
  for (const auto candidate : kSupportedEfforts) {
    if (candidate == effort) {
      supported = true;
      break;
    }
  }
  if (!supported) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "unsupported reasoning effort: ", *reasoning_effort,
             " (supported: none, minimal, low, medium, high, xhigh)");
  }

  if (effort == "none") {
    kwargs["enable_thinking"] = false;
    kwargs.erase("reasoning_effort");
  } else {
    kwargs["enable_thinking"] = true;
    kwargs["reasoning_effort"] = *reasoning_effort;
  }

  return kwargs;
}

}  // namespace fl
