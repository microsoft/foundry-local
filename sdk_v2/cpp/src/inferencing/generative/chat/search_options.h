// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/execution_provider.h"
#include "inferencing/generative/genai_config.h"
#include "util/key_value_pairs.h"

#include <foundry_local/foundry_local_c.h>

#include <optional>
#include <string>
#include <unordered_map>

// Forward declarations — avoid pulling ort_genai.h into the header
struct OgaGeneratorParams;

namespace fl {

/// Parameters extracted from a request that map to ORT GenAI search options.
/// Decoupled from any specific request type so both C API and C++ API can use it.
struct SearchOptions {
  std::optional<float> temperature;
  std::optional<float> top_p;
  std::optional<int> top_k;
  std::optional<int> max_output_tokens;
  std::optional<float> frequency_penalty;
  std::optional<float> presence_penalty;
  std::optional<int> seed;
  std::optional<bool> do_sample;
  std::optional<bool> early_stopping;

  /// Controls whether the model is allowed/required to emit tool calls for this turn.
  /// Wire string ("auto"/"none"/"required") is parsed to the typed enum at the API boundary;
  /// unknown values are rejected with FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT.
  std::optional<flToolChoice> tool_choice;

  /// Additional options from extra_json or passthrough parameters.
  std::unordered_map<std::string, std::string> extra;

  /// Build SearchOptions from a string key-value parameter map.
  /// Keys match FOUNDRY_LOCAL_PARAM_* constants (e.g. "temperature", "max_output_tokens").
  /// Throws fl::Exception on invalid values (e.g. unknown tool_choice).
  static SearchOptions FromParameters(const KeyValuePairs& params);

  /// Parse just the `tool_choice` parameter from a key-value map.
  /// Returns std::nullopt when the key is absent. Throws fl::Exception when present
  /// with a value other than "auto", "none", or "required".
  static std::optional<flToolChoice> ParseToolChoice(const KeyValuePairs& params);
};

/// Apply search options to OgaGeneratorParams.
/// Validates the input + output token budget against model.context_length, falling back to search.max_length.
/// Returns the computed max_length that was set on the params.
/// Throws fl::Exception on invalid configuration (e.g., input too long for model).
///
/// @param options            Search options extracted from the request
/// @param input_token_count  Number of tokens in the encoded prompt
/// @param config             Model's GenAI config (for model.context_length and legacy search.max_length)
/// @param gen_params         ORT GenAI generator params to configure
/// @param ep                 Resolved execution provider. Used to enable chunked prefill by default
///                           on providers that benefit from it (CUDA, NvTensorRtRtx, WebGPU, CPU).
///                           When kDefault, the effective provider is taken from
///                           config.DefaultProvider() (empty ⇒ CPU).
///                           ORT GenAI determines whether the model consumes this option.
/// @param use_full_context   When true, set max_length to the model's full context window
///                           instead of input+output. Used for continuous decoding (cached generators).
/// @param default_max_output_tokens  Default applied when the request does not specify
///                                   max_output_tokens. C# uses 2048 for text, 3072 for vision.
int ApplySearchOptions(const SearchOptions& options,
                       int input_token_count,
                       const GenAIConfig& config,
                       OgaGeneratorParams& gen_params,
                       ExecutionProvider ep,
                       bool use_full_context = false,
                       int default_max_output_tokens = 2048);

}  // namespace fl
