// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

struct OgaGeneratorParams;

namespace fl {

struct ToolCallContext;

/// Applies the shared constrained-decoding policy used by text chat generators.
void ApplyGeneratorGuidance(const ToolCallContext& tool_ctx, OgaGeneratorParams& gen_params);

}  // namespace fl
