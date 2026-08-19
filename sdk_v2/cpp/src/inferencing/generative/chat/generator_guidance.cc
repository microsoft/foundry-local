// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/generator_guidance.h"

#include "inferencing/generative/toolcalling/grammar.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"

#include <ort_genai.h>

#include <stdexcept>
#include <string>

namespace fl {

void ApplyGeneratorGuidance(const ToolCallContext& tool_ctx, OgaGeneratorParams& gen_params) {
  std::string guidance_type;
  std::string guidance_data;

  if (!tool_ctx.guidance_type.empty() && !tool_ctx.guidance_data.empty()) {
    guidance_type = tool_ctx.guidance_type;
    guidance_data = tool_ctx.guidance_data;
  } else {
    std::string json_schema;
    if (tool_ctx.HasTools()) {
      json_schema = BuildToolJsonSchema(tool_ctx);
    }

    guidance_data = BuildLarkGrammar(tool_ctx, json_schema);
    if (!guidance_data.empty()) {
      guidance_type = "lark_grammar";
    }
  }

  const bool tool_call_only = tool_ctx.tool_output && !tool_ctx.text_output;
  if (guidance_type.empty() || guidance_data.empty() || !tool_call_only) {
    return;
  }

  try {
    gen_params.SetGuidance(guidance_type.c_str(), guidance_data.c_str());
  } catch (const std::runtime_error&) {
    // Guidance is optional because not every model/runtime build supports it.
  }
}

}  // namespace fl
