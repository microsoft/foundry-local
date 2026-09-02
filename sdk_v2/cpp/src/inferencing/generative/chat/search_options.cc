// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/search_options.h"
#include "exception.h"

#include <foundry_local/foundry_local_c.h>
#include <ort_genai.h>

#include <algorithm>

namespace fl {

int ApplySearchOptions(const SearchOptions& options,
                       int input_token_count,
                       const GenAIConfig& config,
                       OgaGeneratorParams& gen_params,
                       ExecutionProvider ep,
                       bool use_full_context,
                       int default_max_output_tokens) {
  // Determine model's max context length from genai_config.json search.max_length
  int model_max_length = 0;
  if (config.search.has_value()) {
    model_max_length = config.search->max_length;
  }

  if (model_max_length <= 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "model genai_config.json is missing search.max_length");
  }

  // genai_config.json's search.max_length (read above) is the source of truth for the total input+output budget.
  // The catalog's maxOutputTokens is informational metadata only and is intentionally NOT used to clamp generation:
  // it is commonly a conservative 2048 that would wrongly cap larger contexts (e.g. the 3072 vision default). A
  // user-supplied max_output_tokens is honored as-is and only rejected if input+output exceeds max_length below.
  int max_output = options.max_output_tokens.value_or(default_max_output_tokens);
  if (max_output < 1) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "max_output_tokens must be >= 1");
  }

  // Validate token budget: input + output must not exceed model's max_length
  int total_required = input_token_count + max_output;
  if (total_required > model_max_length) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "request requires " + std::to_string(total_required) + " total tokens (" +
                 std::to_string(input_token_count) + " input + " + std::to_string(max_output) +
                 " output), which exceeds the model's maximum context length of " +
                 std::to_string(model_max_length) + " tokens");
  }

  // max_length in ORT GenAI is the total (input + output) budget.
  // For continuous decoding (cached generators), use the model's full context window
  // so the sequence can grow across turns.
  int effective_max_length = use_full_context
                                 ? model_max_length
                                 : std::min(model_max_length, total_required);
  gen_params.SetSearchOption("max_length", static_cast<double>(effective_max_length));

  // Temperature
  if (options.temperature.has_value()) {
    const float temperature = *options.temperature;
    if (!(temperature >= 0.0f && temperature <= 2.0f)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "temperature must be in the range [0.0, 2.0]");
    }

    gen_params.SetSearchOption("temperature", static_cast<double>(temperature));
  }

  // top_p
  if (options.top_p.has_value()) {
    gen_params.SetSearchOption("top_p", static_cast<double>(*options.top_p));
  }

  // top_k
  if (options.top_k.has_value()) {
    gen_params.SetSearchOption("top_k", static_cast<double>(*options.top_k));
  }

  // Frequency penalty → repetition_penalty in ORT GenAI
  if (options.frequency_penalty.has_value()) {
    gen_params.SetSearchOption("repetition_penalty", static_cast<double>(*options.frequency_penalty));
  }

  // Presence penalty → diversity_penalty in ORT GenAI
  if (options.presence_penalty.has_value()) {
    gen_params.SetSearchOption("diversity_penalty", static_cast<double>(*options.presence_penalty));
  }

  // Random seed
  if (options.seed.has_value()) {
    gen_params.SetSearchOption("random_seed", static_cast<double>(*options.seed));
  }

  // do_sample: if temperature is set and > 0, enable sampling. If temperature == 0, greedy.
  if (options.do_sample.has_value()) {
    gen_params.SetSearchOptionBool("do_sample", *options.do_sample);
  } else if (options.temperature.has_value()) {
    gen_params.SetSearchOptionBool("do_sample", *options.temperature > 0.0f);
  } else {
    // Default: enable sampling (matches C# behavior)
    gen_params.SetSearchOptionBool("do_sample", true);
  }

  // Early stopping — set when stop sequences are present (matches C# behavior)
  if (options.early_stopping.value_or(false)) {
    gen_params.SetSearchOptionBool("early_stopping", true);
  }

  // Preserve a positive model setting. ORT GenAI reports both an absent setting and explicit zero as zero; Foundry
  // Local intentionally treats both as unset. ORT GenAI decides whether the model consumes the resulting option.
  if (gen_params.GetSearchNumber("chunk_size") <= 0) {
    // The model's resolved EP is kDefault for the common load path, so use the provider declared in
    // genai_config.json. An empty provider means ORT's CPU fallback.
    ExecutionProvider effective_ep = ep;
    if (effective_ep == ExecutionProvider::kDefault) {
      std::string config_provider = config.DefaultProvider();
      effective_ep = config_provider.empty() ? ExecutionProvider::kCPU
                                              : EPUtils::StringtoEP(config_provider);
    }

    constexpr double kDefaultChunkSize = 2048.0;
    switch (effective_ep) {
      case ExecutionProvider::kCUDA:
      case ExecutionProvider::kTensorRT_RTX:
      case ExecutionProvider::kWebGPU:
      case ExecutionProvider::kCPU:
        gen_params.SetSearchOption("chunk_size", kDefaultChunkSize);
        break;
      default:
        break;
    }
  }

  return effective_max_length;
}

SearchOptions SearchOptions::FromParameters(const KeyValuePairs& params) {
  SearchOptions opts;

  auto try_float = [&](const std::string& key) -> std::optional<float> {
    auto it = params.find(key);
    if (it != params.end()) {
      return std::stof(it->second);
    }

    return std::nullopt;
  };

  auto try_int = [&](const std::string& key) -> std::optional<int> {
    auto it = params.find(key);
    if (it != params.end()) {
      return std::stoi(it->second);
    }

    return std::nullopt;
  };

  opts.temperature = try_float(FOUNDRY_LOCAL_PARAM_TEMPERATURE);
  if (opts.temperature.has_value() && !(*opts.temperature >= 0.0f && *opts.temperature <= 2.0f)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "temperature must be in the range [0.0, 2.0]");
  }

  opts.top_p = try_float(FOUNDRY_LOCAL_PARAM_TOP_P);
  opts.top_k = try_int(FOUNDRY_LOCAL_PARAM_TOP_K);
  opts.max_output_tokens = try_int(FOUNDRY_LOCAL_PARAM_MAX_OUTPUT_TOKENS);
  opts.frequency_penalty = try_float(FOUNDRY_LOCAL_PARAM_FREQUENCY_PENALTY);
  opts.presence_penalty = try_float(FOUNDRY_LOCAL_PARAM_PRESENCE_PENALTY);
  opts.seed = try_int(FOUNDRY_LOCAL_PARAM_SEED);

  auto try_bool = [&](const std::string& key) -> std::optional<bool> {
    auto it = params.find(key);
    if (it != params.end()) {
      return it->second == "true" || it->second == "1";
    }

    return std::nullopt;
  };

  opts.do_sample = try_bool(FOUNDRY_LOCAL_PARAM_DO_SAMPLE);
  opts.early_stopping = try_bool(FOUNDRY_LOCAL_PARAM_EARLY_STOPPING);
  opts.tool_choice = ParseToolChoice(params);

  return opts;
}

std::optional<flToolChoice> SearchOptions::ParseToolChoice(const KeyValuePairs& params) {
  auto it = params.find(FOUNDRY_LOCAL_PARAM_TOOL_CHOICE);
  if (it == params.end()) {
    return std::nullopt;
  }

  const std::string& value = it->second;
  if (value == "auto") {
    return FOUNDRY_LOCAL_TOOL_CHOICE_AUTO;
  } else if (value == "none") {
    return FOUNDRY_LOCAL_TOOL_CHOICE_NONE;
  } else if (value == "required") {
    return FOUNDRY_LOCAL_TOOL_CHOICE_REQUIRED;
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
           "Invalid value for tool_choice: '" + value + "'. Expected 'auto', 'none', or 'required'.");
}

}  // namespace fl
