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
#include <vector>

// Forward declarations — avoid pulling ort_genai.h into the header
struct OgaGeneratorParams;

namespace fl {

struct ToolCallContext;

struct TurnGuidanceOptions {
  std::string type;
  std::string data;
  bool user_specified = false;
};

/// Caller-expressed sampling settings, normalized to a combination ORT GenAI accepts.
///
/// ORT GenAI resolves a turn as `model search defaults + explicit overrides` and calls the result greedy when
/// `!do_sample || top_k == 1 || temperature == 0` (`Generators::EffectiveTurnPolicy::IsGreedy`). A greedy turn draws
/// from no distribution, so `Generators::ValidateTurnPolicy` rejects any *explicitly set* scalar it would ignore: a
/// temperature other than 0 or 1, a top_p strictly inside (0, 1), or a top_k above 1. Scalars the caller never set are
/// left unset here so the model's own defaults keep deciding — materializing a Foundry default would silently override
/// model policy and can itself be rejected upstream.
struct SamplingPlan {
  std::optional<bool> do_sample;
  std::optional<float> temperature;
  std::optional<float> top_p;
  std::optional<int> top_k;

  /// True when the caller's own settings already force top-logit selection.
  bool greedy = false;
};

/// Per-turn Engine settings for the newer ORT GenAI `OgaTurnOptions` surface.
/// The output limit is mandatory after request preparation. Optional sampling fields stay at the
/// model's own default for that turn when absent.
struct EngineTurnOptionsPlan {
  int max_generated_tokens = 0;
  SamplingPlan sampling;
  std::optional<int> seed;
  std::vector<std::string> stop_sequences;
  std::optional<TurnGuidanceOptions> guidance;
};

/// Parameters extracted from a request that map to ORT GenAI search options.
/// Decoupled from any specific request type so both C API and C++ API can use it.
struct SearchOptions {
  std::optional<float> temperature;
  std::optional<float> top_p;
  std::optional<int> top_k;
  std::optional<int> max_output_tokens;
  std::optional<float> frequency_penalty;  // Currently only the neutral value 0 is supported.
  std::optional<float> presence_penalty;   // Currently only the neutral value 0 is supported.
  std::optional<int> seed;
  std::optional<bool> do_sample;
  std::optional<bool> early_stopping;  // Beam-search policy; unsupported by Engine backends.
  std::vector<std::string> stop_sequences;

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

  /// Whether settings baked into retained backend state match another turn.
  bool HasSameRetainedGenerationSettings(const SearchOptions& other, ChatBackendKind backend_kind) const;
};

inline constexpr int kDefaultChatTextMaxOutputTokens = 2048;
inline constexpr int kDefaultChatMediaMaxOutputTokens = 3072;

/// Resolve the request output limit from the package schema and host fallback policy.
///
/// The current GenAIConfig schema has no distinct package generation default:
/// model.context_length and search.max_length are total-context limits. Neither participates
/// in this resolution. If a distinct generation field is added later, it belongs between the
/// explicit request value and the text/media fallback here.
int ResolveOutputLimit(const SearchOptions& options, bool has_media);

/// Return positive model.context_length, falling back to positive search.max_length for legacy artifacts.
int64_t GetModelMaxContextLength(const GenAIConfig& config);

/// Resolve the guidance configuration that should apply to a single tool-only turn.
std::optional<TurnGuidanceOptions> ResolveTurnGuidanceOptions(const ToolCallContext& tool_ctx,
                                                              bool prompt_opens_reasoning);

/// Normalize caller-expressed sampling settings for one turn. See SamplingPlan.
/// Throws fl::Exception when a value is outside the range ORT GenAI accepts for any turn.
SamplingPlan ResolveSamplingPlan(const SearchOptions& options);

/// Whether Engine turn options may carry stop strings for this backend.
bool ShouldForwardStopSequencesToEngine(ChatBackendKind backend_kind);

/// Whether Engine turn options may carry a per-turn seed for this backend.
bool SupportsPerTurnSeed(ChatBackendKind backend_kind);

/// Resolve SearchOptions + ToolCallContext into the per-turn Engine settings used by newer OGA headers.
/// max_output_tokens must already have been resolved during request preparation.
/// Throws fl::Exception when the request is unresolved or asks for something this backend cannot honor per turn.
EngineTurnOptionsPlan BuildEngineTurnOptionsPlan(const SearchOptions& options,
                                                 const ToolCallContext& tool_ctx,
                                                 ChatBackendKind backend_kind,
                                                 bool prompt_opens_reasoning);

/// Apply search options to OgaGeneratorParams.
/// Validates token budget (input + output vs model max_length from config).
/// Returns the computed max_length that was set on the params.
/// Throws fl::Exception on invalid configuration (e.g., input too long for model).
///
/// @param options            Search options extracted from the request
/// @param input_token_count  Number of tokens in the encoded prompt
/// @param config             Model's GenAI config (model.context_length, then search.max_length fallback)
/// @param gen_params         ORT GenAI generator params to configure
/// @param ep                 Resolved execution provider. Used to enable chunked prefill by default
///                           on providers that benefit from it (CUDA, NvTensorRtRtx, WebGPU, CPU).
///                           When kDefault, the effective provider is taken from
///                           config.DefaultProvider() (empty ⇒ CPU).
///                           ORT GenAI determines whether the model consumes this option.
/// @param use_full_context   When true, set max_length to the model's full context window
///                           instead of input+output. Used for continuous decoding (cached generators).
/// @param has_media          Whether to use the media fallback when called outside prepared-request submission.
int64_t ApplySearchOptions(const SearchOptions& options,
                           int64_t input_token_count,
                           const GenAIConfig& config,
                           OgaGeneratorParams& gen_params,
                           ExecutionProvider ep,
                           bool use_full_context = false,
                           bool has_media = false);

/// Applies request-level grammar guidance to generator parameters when the tool context requires tool-only output.
void ApplyGuidanceOptions(const ToolCallContext& tool_ctx,
                          bool prompt_opens_reasoning,
                          OgaGeneratorParams& gen_params);

}  // namespace fl
