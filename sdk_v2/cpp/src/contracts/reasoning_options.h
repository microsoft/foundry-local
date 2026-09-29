// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace fl {

/// Merge a standard OpenAI reasoning effort into model chat-template kwargs.
/// `none` disables thinking; supported nonzero efforts enable thinking and set reasoning_effort.
///
/// @throws fl::Exception FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT for a non-object kwargs value or unknown effort.
nlohmann::json ResolveReasoningTemplateKwargs(
    std::optional<nlohmann::json> template_kwargs,
    const std::optional<std::string>& reasoning_effort);

}  // namespace fl
