// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/chat/search_options.h"
#include "exception.h"
#include "inferencing/generative/chat/stop_strings.h"
#include "inferencing/generative/toolcalling/grammar.h"
#include "inferencing/generative/toolcalling/tool_call_context.h"

#include <foundry_local/foundry_local_c.h>
#include <ort_genai.h>

#include <algorithm>
#include <cmath>

namespace fl {

namespace {

void ValidateTemperature(float temperature) {
  if (!(temperature >= 0.0f && temperature <= 2.0f)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "temperature must be in the range [0.0, 2.0]");
  }
}

void ValidateTopP(float top_p) {
  if (!std::isfinite(top_p) || top_p < 0.0f || top_p > 1.0f) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "top_p must be finite and in the range [0.0, 1.0]");
  }
}

void ValidateTopK(int top_k) {
  if (top_k < 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "top_k must be 0 or greater");
  }
}

void ValidatePenalties(const SearchOptions& options) {
  if (options.frequency_penalty.value_or(0.0f) != 0.0f) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "nonzero frequency_penalty is not supported; ORT repetition_penalty has different semantics");
  }

  if (options.presence_penalty.value_or(0.0f) != 0.0f) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "nonzero presence_penalty is not supported; ORT diversity_penalty has different semantics");
  }
}

}  // namespace

int ResolveMaxOutputTokens(const SearchOptions& options, int default_max_output_tokens) {
  const int max_output = options.max_output_tokens.value_or(default_max_output_tokens);
  if (max_output < 1) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "max_output_tokens must be >= 1");
  }

  return max_output;
}

int GetModelMaxContextLength(const GenAIConfig& config) {
  int model_max_length = 0;
  if (config.search.has_value()) {
    model_max_length = config.search->max_length;
  }

  if (model_max_length <= 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "model genai_config.json is missing search.max_length");
  }

  return model_max_length;
}

std::optional<TurnGuidanceOptions> ResolveTurnGuidanceOptions(const ToolCallContext& tool_ctx,
                                                              bool prompt_opens_reasoning) {
  std::string guidance_type;
  std::string guidance_data;
  const bool user_specified_guidance =
      !tool_ctx.guidance_type.empty() && !tool_ctx.guidance_data.empty();

  if (user_specified_guidance) {
    guidance_type = tool_ctx.guidance_type;
    guidance_data = tool_ctx.guidance_data;
  } else {
    std::string json_schema;
    if (tool_ctx.HasTools()) {
      json_schema = BuildToolJsonSchema(tool_ctx);
    }

    guidance_data = BuildLarkGrammar(tool_ctx, json_schema, prompt_opens_reasoning);
    if (!guidance_data.empty()) {
      guidance_type = "lark_grammar";
    }
  }

  const bool tool_call_only = tool_ctx.tool_output && !tool_ctx.text_output;
  if (!guidance_type.empty() && !guidance_data.empty() &&
      (user_specified_guidance || tool_call_only)) {
    return TurnGuidanceOptions{
        std::move(guidance_type),
        std::move(guidance_data),
        user_specified_guidance,
    };
  }

  return std::nullopt;
}

SamplingPlan ResolveSamplingPlan(const SearchOptions& options) {
  if (options.temperature.has_value()) {
    ValidateTemperature(*options.temperature);
  }

  if (options.top_p.has_value()) {
    ValidateTopP(*options.top_p);
  }

  if (options.top_k.has_value()) {
    ValidateTopK(*options.top_k);
  }

  SamplingPlan plan;
  plan.do_sample = options.do_sample;
  if (!plan.do_sample.has_value() && options.temperature.has_value()) {
    plan.do_sample = *options.temperature > 0.0f;
  }
  plan.temperature = options.temperature;
  plan.top_p = options.top_p;
  plan.top_k = options.top_k;
  // Mirrors EffectiveTurnPolicy::IsGreedy() restricted to what the caller actually spelled out. The model's own
  // search defaults can force greedy too, but Foundry cannot observe them, so only the caller's settings are used.
  plan.greedy = plan.do_sample == false || options.temperature == 0.0f || options.top_k == 1;

  if (!plan.greedy) {
    return plan;
  }

  // Drop only the explicitly set scalars a greedy turn would ignore, matching ValidateTurnPolicy's contradiction
  // rules. Neutral values are kept because honoring them exactly is what a greedy turn already does: temperature 0
  // (and 1, which rescales nothing), top_p 0 or 1, and top_k 0 or 1.
  if (plan.temperature.has_value() && *plan.temperature != 0.0f && *plan.temperature != 1.0f) {
    plan.temperature.reset();
  }

  if (plan.top_p.has_value() && *plan.top_p > 0.0f && *plan.top_p < 1.0f) {
    plan.top_p.reset();
  }

  if (plan.top_k.has_value() && *plan.top_k > 1) {
    plan.top_k.reset();
  }

  return plan;
}

bool ShouldForwardStopSequencesToEngine(ChatBackendKind backend_kind) {
  return backend_kind == ChatBackendKind::kEngine;
}

bool SupportsPerTurnSeed(ChatBackendKind backend_kind) {
  return backend_kind == ChatBackendKind::kEngine;
}

EngineTurnOptionsPlan BuildEngineTurnOptionsPlan(const SearchOptions& options,
                                                 const ToolCallContext& tool_ctx,
                                                 ChatBackendKind backend_kind,
                                                 bool prompt_opens_reasoning) {
  ValidatePenalties(options);
  if (options.early_stopping.value_or(false)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "early_stopping is not supported by Engine backends; it is a beam-search option and Engine "
             "uses single-beam decoding");
  }

  EngineTurnOptionsPlan plan;
  if (options.max_output_tokens.has_value() && *options.max_output_tokens < 1) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "max_output_tokens must be >= 1");
  }
  plan.max_generated_tokens = options.max_output_tokens;
  plan.sampling = ResolveSamplingPlan(options);

  // Negative seeds preserve classic ORT GenAI's nondeterministic behavior and require no per-turn seed support.
  // Nonnegative seeds must be forwarded because zero is a valid deterministic seed.
  if (options.seed.has_value() && *options.seed >= 0) {
    plan.seed = *options.seed;
  }

  if (ShouldForwardStopSequencesToEngine(backend_kind)) {
    plan.stop_sequences = options.stop_sequences;
  }

  plan.guidance = ResolveTurnGuidanceOptions(tool_ctx, prompt_opens_reasoning);
  return plan;
}

void ApplyGuidanceOptions(const ToolCallContext& tool_ctx,
                          bool prompt_opens_reasoning,
                          OgaGeneratorParams& gen_params) {
  if (const auto guidance = ResolveTurnGuidanceOptions(tool_ctx, prompt_opens_reasoning)) {
    try {
      gen_params.SetGuidance(guidance->type.c_str(), guidance->data.c_str());
    } catch (const std::runtime_error& e) {
      if (guidance->user_specified) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                 "failed to apply requested response guidance: " + std::string(e.what()));
      }

      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "failed to apply required tool-call guidance: " + std::string(e.what()));
    }
  }
}

int ApplySearchOptions(const SearchOptions& options,
                       int input_token_count,
                       const GenAIConfig& config,
                       OgaGeneratorParams& gen_params,
                       ExecutionProvider ep,
                       bool use_full_context,
                       int default_max_output_tokens) {
  ValidatePenalties(options);

  const int model_max_length = GetModelMaxContextLength(config);

  // genai_config.json's search.max_length (read above) is the source of truth for the total input+output budget.
  // The catalog's maxOutputTokens is informational metadata only and is intentionally NOT used to clamp generation:
  // it is commonly a conservative 2048 that would wrongly cap larger contexts (e.g. the 3072 vision default). A
  // user-supplied max_output_tokens is honored as-is and only rejected if input+output exceeds max_length below.
  const int max_output = ResolveMaxOutputTokens(options, default_max_output_tokens);

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

  // One shared normalization for every backend: the same combination is forwarded to a classic generator, to an
  // Engine request built from params, and to per-turn Engine options.
  const SamplingPlan sampling = ResolveSamplingPlan(options);

  if (sampling.temperature.has_value()) {
    gen_params.SetSearchOption("temperature", static_cast<double>(*sampling.temperature));
  }

  if (sampling.top_p.has_value()) {
    gen_params.SetSearchOption("top_p", static_cast<double>(*sampling.top_p));
  }

  if (sampling.top_k.has_value()) {
    gen_params.SetSearchOption("top_k", static_cast<double>(*sampling.top_k));
  }

  // Random seed
  if (options.seed.has_value()) {
    gen_params.SetSearchOption("random_seed", static_cast<double>(*options.seed));
  }

  // Preserve the established Generator default of sampling when the caller supplies no sampling policy.
  if (sampling.do_sample.has_value()) {
    gen_params.SetSearchOptionBool("do_sample", *sampling.do_sample);
  } else {
    gen_params.SetSearchOptionBool("do_sample", true);
  }

  // Early stopping is a beam-search policy, independent from decoded stop strings.
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
  if (opts.temperature.has_value()) {
    ValidateTemperature(*opts.temperature);
  }

  opts.top_p = try_float(FOUNDRY_LOCAL_PARAM_TOP_P);
  opts.top_k = try_int(FOUNDRY_LOCAL_PARAM_TOP_K);
  opts.max_output_tokens = try_int(FOUNDRY_LOCAL_PARAM_MAX_OUTPUT_TOKENS);
  opts.frequency_penalty = try_float(FOUNDRY_LOCAL_PARAM_FREQUENCY_PENALTY);
  opts.presence_penalty = try_float(FOUNDRY_LOCAL_PARAM_PRESENCE_PENALTY);
  opts.seed = try_int(FOUNDRY_LOCAL_PARAM_SEED);
  opts.stop_sequences = LoadStopStringsOption(params);

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

bool SearchOptions::HasSameRetainedGenerationSettings(const SearchOptions& other,
                                                      ChatBackendKind backend_kind) const {
  if (backend_kind != ChatBackendKind::kGenerator) {
    // Engine-supported settings are supplied on each BeginTurn. Static Engine state is rebuilt unconditionally.
    return true;
  }

  return temperature == other.temperature && top_p == other.top_p && top_k == other.top_k &&
         frequency_penalty.value_or(0.0f) == other.frequency_penalty.value_or(0.0f) &&
         presence_penalty.value_or(0.0f) == other.presence_penalty.value_or(0.0f) &&
         seed == other.seed && do_sample == other.do_sample &&
         early_stopping.value_or(false) == other.early_stopping.value_or(false) && extra == other.extra;
}

}  // namespace fl
