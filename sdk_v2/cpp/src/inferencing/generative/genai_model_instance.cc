// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/genai_model_instance.h"
#include "exception.h"
#include "inferencing/execution_provider.h"
#include "inferencing/generative/chat/onnx_chat_engine.h"
#include "util/key_value_pairs.h"
#include "utils.h"

#include <ort_genai.h>

#include <fmt/format.h>
#include <algorithm>
#include <set>

namespace fl {
namespace {

constexpr std::string_view kQwen35TextModelType = "qwen3_5_text";
constexpr const char* kToolCallProbeMessages = R"([
  {"role":"user","content":"capability-probe"},
  {"role":"assistant","content":"","tool_calls":[
    {"id":"probe-call","type":"function","function":{
      "name":"probe_function","arguments":{"probe_parameter":"probe_value"}
    }}
  ]}
])";
constexpr const char* kToolResultProbeMessages = R"([
  {"role":"user","content":"capability-probe"},
  {"role":"assistant","content":"","tool_calls":[
    {"id":"probe-call-a","type":"function","function":{"name":"probe_first","arguments":{}}},
    {"id":"probe-call-b","type":"function","function":{"name":"probe_second","arguments":{}}}
  ]},
  {"role":"tool","tool_call_id":"probe-call-b","content":"probe-result-first"},
  {"role":"tool","tool_call_id":"probe-call-a","content":"probe-result-second"}
])";
constexpr const char* kToolProbeDefinitions = R"([
  {"type":"function","function":{
    "name":"probe_function",
    "description":"Capability probe",
    "parameters":{
      "type":"object",
      "properties":{"probe_parameter":{"type":"string"}},
      "required":["probe_parameter"]
    }
  }},
  {"type":"function","function":{
    "name":"probe_first",
    "description":"Capability probe",
    "parameters":{"type":"object","properties":{}}
  }},
  {"type":"function","function":{
    "name":"probe_second",
    "description":"Capability probe",
    "parameters":{"type":"object","properties":{}}
  }}
])";
constexpr const char* kReasoningProbeMessages = R"([
  {"role":"user","content":"reasoning-probe-user-a"},
  {"role":"assistant","reasoning_content":"reasoning-probe-private-a","content":"reasoning-probe-visible-a"},
  {"role":"user","content":"reasoning-probe-user-b"},
  {"role":"assistant","reasoning_content":"reasoning-probe-private-b","content":"reasoning-probe-visible-b"},
  {"role":"user","content":"reasoning-probe-user-c"}
])";
constexpr const char* kDisableReasoningPreservation = R"({"preserve_thinking":false})";

constexpr std::string_view kExpectedToolCallBlock =
    "<tool_call>\n"
    "<function=probe_function>\n"
    "<parameter=probe_parameter>\n"
    "probe_value\n"
    "</parameter>\n"
    "</function>\n"
    "</tool_call>";

bool ContainsInOrder(std::string_view text, std::initializer_list<std::string_view> values) {
  size_t offset = 0;
  for (const auto value : values) {
    const size_t found = text.find(value, offset);
    if (found == std::string_view::npos) {
      return false;
    }
    offset = found + value.size();
  }
  return true;
}

struct DelimitedRegion {
  size_t end = 0;
  std::string_view body;
};

std::vector<DelimitedRegion> FindDelimitedRegions(std::string_view text,
                                                  std::string_view opening,
                                                  std::string_view closing) {
  std::vector<DelimitedRegion> regions;
  if (opening.empty() || closing.empty() || opening == closing) {
    return regions;
  }

  size_t offset = 0;
  while (true) {
    const size_t begin = text.find(opening, offset);
    if (begin == std::string_view::npos) {
      break;
    }

    const size_t body_begin = begin + opening.size();
    const size_t end = text.find(closing, body_begin);
    if (end == std::string_view::npos) {
      return {};
    }

    const size_t nested = text.find(opening, body_begin);
    if (nested != std::string_view::npos && nested < end) {
      return {};
    }

    regions.push_back({end + closing.size(), text.substr(body_begin, end - body_begin)});
    offset = end + closing.size();
  }
  return regions;
}

std::optional<HarmonyProtocol> QualifyHarmonyProtocol(std::string_view model_type, Preprocessor& preprocessor) {
  if (model_type != "gptoss") return std::nullopt;
#if !FOUNDRY_LOCAL_OGA_HAS_TERMINAL_TOKEN
  (void)preprocessor;
  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
           "GPT-OSS Harmony requires an ORT GenAI runtime exposing sampled terminal-token identity");
#else
  const auto resolve = [&](const char* spelling) {
    const auto encoded = preprocessor.Encode(spelling);
    const auto literal = preprocessor.EncodeLiteral(spelling);
    if (encoded->SequenceCount(0) != 1 || encoded->SequenceData(0)[0] < 0 ||
        (literal->SequenceCount(0) == 1 && literal->SequenceData(0)[0] == encoded->SequenceData(0)[0])) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
               "Harmony control must have distinct literal/control encoding: ", spelling);
    }
    return encoded->SequenceData(0)[0];
  };
  HarmonyProtocol protocol{
      resolve("<|start|>"),
      resolve("<|channel|>"),
      resolve("<|message|>"),
      resolve("<|end|>"),
      resolve("<|call|>"),
      resolve("<|return|>"),
      resolve("<|constrain|>"),
      {},
  };
  const std::set<int32_t> controls{protocol.start, protocol.channel, protocol.message, protocol.end,
                                   protocol.handoff, protocol.answer, protocol.format};
  const auto& eos = preprocessor.GetEosTokenIds();
  if (controls.size() != 7 || std::find(eos.begin(), eos.end(), protocol.handoff) == eos.end() ||
      std::find(eos.begin(), eos.end(), protocol.answer) == eos.end() ||
      std::find(eos.begin(), eos.end(), protocol.end) != eos.end()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Invalid Harmony control-token or EOS configuration");
  }
  constexpr const char* probe = R"([
    {"role":"user","content":"probe"},
    {"role":"assistant","content":"","thinking":"probe-private","tool_calls":[
      {"function":{"name":"probe_function","arguments":{},"arguments_json":"__probe_arguments__"}}
    ]},
    {"role":"tool","name":"probe_function","content":"","content_json":"__probe_result__"}
  ])";
  const auto rendered = preprocessor.ApplyChatTemplate(probe, nullptr, true);
  if (!ContainsInOrder(rendered, {"<|channel|>analysis<|message|>probe-private<|end|>",
                                  "<|start|>assistant to=functions.probe_function<|channel|>commentary json"
                                  "<|message|>__probe_arguments__<|call|>",
                                  "<|start|>functions.probe_function to=assistant<|channel|>commentary"
                                  "<|message|>__probe_result__<|end|>"}) ||
      !rendered.ends_with(HarmonyProtocol::kAssistantPrefix)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "GPT-OSS requires the qualified protected Harmony transcript template");
  }
  constexpr const char* custom_probe = R"([
    {"role":"user","content":"probe"},
    {"role":"assistant","content":"","tool_calls":[
      {"function":{"name":"probe_custom","native_freeform":true,
                   "arguments":{},"arguments_json":"__probe_raw_input__"}}
    ]},
    {"role":"tool","name":"probe_custom","content":"","content_json":"__probe_custom_result__"}
  ])";
  constexpr const char* custom_tools = R"([
    {"type":"function","function":{"name":"probe_custom","description":"probe",
                                  "native_freeform":true,"parameters":{}}}
  ])";
  const auto custom_rendered = preprocessor.ApplyChatTemplate(custom_probe, custom_tools, true);
  if (!ContainsInOrder(custom_rendered, {"type probe_custom = (_: FREEFORM) => any;",
                                         "<|start|>assistant to=functions.probe_custom<|channel|>commentary"
                                         "<|message|>__probe_raw_input__<|call|>",
                                         "<|start|>functions.probe_custom to=assistant<|channel|>commentary"
                                         "<|message|>__probe_custom_result__<|end|>"}) ||
      !custom_rendered.ends_with(HarmonyProtocol::kAssistantPrefix)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "GPT-OSS requires protected native custom-tool declarations and history");
  }
  const auto prefix = preprocessor.Encode(HarmonyProtocol::kAssistantPrefix);
  protocol.assistant_prefix_tokens.assign(prefix->SequenceData(0), prefix->SequenceData(0) + prefix->SequenceCount(0));
  return protocol;
#endif
}

ModelCapabilities ResolveModelCapabilities(std::string_view model_type, Preprocessor& preprocessor,
                                           std::string_view tool_call_start,
                                           std::string_view tool_call_end,
                                           std::string_view reasoning_start,
                                           std::string_view reasoning_end) noexcept {
#if !FOUNDRY_LOCAL_OGA_HAS_CHAT_TEMPLATE_KWARGS
  (void)reasoning_start;
  (void)reasoning_end;
#endif

  ModelCapabilities capabilities;
  if (!tool_call_start.empty() && !tool_call_end.empty() && tool_call_start != tool_call_end) {
    std::string tool_call_projection;
    try {
      tool_call_projection = preprocessor.ApplyChatTemplate(
          kToolCallProbeMessages, kToolProbeDefinitions, /*add_generation_prompt=*/false);
    } catch (...) {
      // Tool-call projection failed closed without disabling independent reasoning probing.
    }

    std::string tool_result_projection;
    try {
      tool_result_projection = preprocessor.ApplyChatTemplate(
          kToolResultProbeMessages, kToolProbeDefinitions, /*add_generation_prompt=*/false);
    } catch (...) {
      // Tool-result projection is optional; a single-call-capable template may reject parallel history.
    }

    capabilities = model_capabilities_internal::ResolveRenderedProbes(
        model_type, tool_call_projection, tool_result_projection, tool_call_start, tool_call_end);
  }

  try {
    const auto reasoning_projection = preprocessor.ApplyChatTemplateWithOptions(
      kReasoningProbeMessages, /*tools_json=*/nullptr, /*template_kwargs_json=*/nullptr,
      /*add_generation_prompt=*/false);
    const auto no_preserve_reasoning_projection = preprocessor.ApplyChatTemplateWithOptions(
      kReasoningProbeMessages, /*tools_json=*/nullptr, kDisableReasoningPreservation,
      /*add_generation_prompt=*/false);
    const auto reasoning_capabilities = model_capabilities_internal::ResolveRenderedProbes(
      model_type, {}, {}, {}, {}, reasoning_projection, no_preserve_reasoning_projection,
      reasoning_start, reasoning_end);
    capabilities.supports_reasoning_history = reasoning_capabilities.supports_reasoning_history;
    capabilities.supports_preserve_thinking = reasoning_capabilities.supports_preserve_thinking;
    capabilities.supports_reasoning_controls = true;
  } catch (...) {
    capabilities.supports_reasoning_history = false;
    capabilities.supports_preserve_thinking = false;
    capabilities.supports_reasoning_controls = false;
  }

  return capabilities;
}

}  // namespace

ModelCapabilities model_capabilities_internal::ResolveRenderedProbes(
    std::string_view model_type,
    std::string_view tool_call_projection,
    std::string_view tool_result_projection,
    std::string_view tool_call_start,
    std::string_view tool_call_end,
    std::string_view reasoning_projection,
    std::string_view no_preserve_reasoning_projection,
    std::string_view reasoning_start,
    std::string_view reasoning_end) noexcept {
  const auto tool_call_regions = FindDelimitedRegions(
      tool_call_projection, tool_call_start, tool_call_end);
  const bool supports_tool_calling = !tool_call_start.empty() && !tool_call_end.empty() &&
                                     tool_call_start != tool_call_end &&
                                     std::ranges::any_of(tool_call_regions, [](const auto& region) {
                                       return ContainsInOrder(
                                           region.body, {"probe_function", "probe_parameter", "probe_value"});
                                     });
  const bool is_qwen35_text = model_type == kQwen35TextModelType;
  const bool native_qwen_xml_tool_calls = supports_tool_calling && is_qwen35_text &&
                                          tool_call_projection.find(kExpectedToolCallBlock) != std::string_view::npos;
  const auto tool_result_regions = FindDelimitedRegions(
      tool_result_projection, tool_call_start, tool_call_end);
  const auto first_call = std::ranges::find_if(tool_result_regions, [](const auto& region) {
    return region.body.find("probe_first") != std::string_view::npos;
  });
  const auto second_call = first_call == tool_result_regions.end()
                               ? tool_result_regions.end()
                               : std::ranges::find_if(first_call + 1, tool_result_regions.end(), [](const auto& region) {
                                   return region.body.find("probe_second") != std::string_view::npos;
                                 });
  const bool positional_tool_results = supports_tool_calling &&
                                       second_call != tool_result_regions.end() &&
                                       ContainsInOrder(tool_result_projection.substr(second_call->end),
                                                       {"probe-result-first", "probe-result-second"}) &&
                                       tool_result_projection.find("probe-call-a") == std::string_view::npos &&
                                       tool_result_projection.find("probe-call-b") == std::string_view::npos;

  const bool supports_reasoning_history = !reasoning_start.empty() && !reasoning_end.empty() &&
      ContainsInOrder(reasoning_projection,
                      {reasoning_start, "reasoning-probe-private-a", reasoning_end,
                       "reasoning-probe-visible-a", reasoning_start, "reasoning-probe-private-b",
                       reasoning_end, "reasoning-probe-visible-b"});
  const bool supports_preserve_thinking = supports_reasoning_history &&
      no_preserve_reasoning_projection.find("reasoning-probe-private-a") == std::string_view::npos &&
      no_preserve_reasoning_projection.find("reasoning-probe-private-b") == std::string_view::npos &&
      ContainsInOrder(no_preserve_reasoning_projection,
                      {"reasoning-probe-visible-a", "reasoning-probe-visible-b"});

  return {supports_tool_calling, native_qwen_xml_tool_calls, positional_tool_results,
          supports_reasoning_history, supports_preserve_thinking,
          !reasoning_projection.empty() || !no_preserve_reasoning_projection.empty()};
}

// ---------------------------------------------------------------------------
// Constructors / Destructors
// ---------------------------------------------------------------------------

GenAIModelInstance::GenAIModelInstance(std::string model_id,
                                       std::string effective_model_path,
                                       GenAIConfig genai_config,
                                       ExecutionProvider resolved_ep,
                                       ILogger& logger)
    : model_id_(std::move(model_id)),
      model_path_(std::move(effective_model_path)),
      genai_config_(std::move(genai_config)),
      ep_(resolved_ep),
      logger_(logger),
      last_activity_(std::chrono::steady_clock::now()) {
  // Create OGA Config from the effective model directory
  std::unique_ptr<OgaConfig> oga_config;
  try {
    oga_config = OgaConfig::Create(model_path_.c_str());
  } catch (const std::runtime_error& e) {
    FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL,
                     "failed to create OGA config for model ", model_id_, ": ", e.what());
  }

  // Every explicit EP overrides the main decoder provider from genai_config.json. CPU is OGA's default when the
  // provider list is empty, and EPtoGenAI intentionally has no CPU name, so CPU clears the list without appending a
  // provider. DFlash has separate provider options and is not changed by these main decoder overrides.
  if (ep_ != ExecutionProvider::kDefault) {
    try {
      logger_.Log(LogLevel::Information,
                  fmt::format("model {} applying explicit main decoder EP override: {}", model_id_,
                              EPUtils::EPtoRegistrationName(ep_)));
      oga_config->ClearProviders();
      if (ep_ != ExecutionProvider::kCPU) {
        std::string_view provider_str = EPUtils::EPtoGenAI(ep_);
        oga_config->AppendProvider(provider_str.data());
      }

      // Preserve existing behavior: an explicit CUDA override disables graph capture for the main decoder only.
      if (ep_ == ExecutionProvider::kCUDA) {
        oga_config->SetProviderOption("cuda", "enable_cuda_graph", "0");
        logger_.Log(LogLevel::Information,
                    fmt::format("model {} explicit CUDA override set main decoder enable_cuda_graph=0; "
                                "DFlash provider options are unchanged",
                                model_id_));
      }
    } catch (const std::runtime_error& e) {
      FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL,
                       "failed to configure EP for model ", model_id_, ": ", e.what());
    }
  } else {
    const auto configured_provider = genai_config_.DefaultProvider();
    logger_.Log(LogLevel::Information,
                fmt::format("model {} using main decoder provider options from genai_config.json (provider={})",
                            model_id_, configured_provider.empty() ? "implicit CPU" : configured_provider));
  }

  // Create OGA Model
  try {
    oga_model_ = OgaModel::Create(*oga_config);
    const OgaString model_type = oga_model_->GetType();
    const auto* model_type_text = static_cast<const char*>(model_type);
    model_type_ = model_type_text == nullptr ? std::string{} : std::string{model_type_text};
  } catch (const std::runtime_error& e) {
    FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL,
                     "failed to load model ", model_id_, ": ", e.what());
  }

  try {
    preprocessor_ = Preprocessor::Create(*oga_model_, IsMultiModal());
  } catch (const std::runtime_error& e) {
    FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL,
                     "failed to create preprocessor for model ", model_id_, ": ", e.what());
  }

  const auto& tag_info = GetTagInfo();
  capabilities_ = ResolveModelCapabilities(model_type_, *preprocessor_,
                                           tag_info.bot_str, tag_info.eot_str,
                                           tag_info.bor_str, tag_info.eor_str);
  capabilities_.harmony = QualifyHarmonyProtocol(model_type_, *preprocessor_);
  if (capabilities_.harmony) {
    capabilities_.supports_tool_calling = true;
    capabilities_.supports_reasoning_history = true;
    capabilities_.positional_tool_results = true;
    capabilities_.supports_reasoning_controls = true;
  }

  if (IsMultiModal() && genai_config_.GetChatBackendKind() == ChatBackendKind::kEngine) {
    FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL,
                     "model ", model_id_,
                     " declares an Engine backend, but Engine is not supported for multimodal models");
  }

  if (genai_config_.GetChatBackendKind() == ChatBackendKind::kEngine) {
    try {
      chat_engine_ = std::make_unique<OnnxChatEngine>(*this);
    } catch (const std::runtime_error& e) {
      FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL,
                       "failed to create chat engine for model ", model_id_, ": ", e.what());
    }
  }
}

// Destructor: unique_ptr members are destroyed in reverse declaration order.
// OGA objects have custom operator delete that calls OgaDestroy* functions.
// Destruction order: chat engine → preprocessor → OGA model (correct: dependents first).
GenAIModelInstance::~GenAIModelInstance() = default;

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

bool GenAIModelInstance::IsMultiModal() const {
  return genai_config_.model.has_value() && genai_config_.model->IsMultiModal();
}

OgaModel& GenAIModelInstance::GetOgaModel() {
  if (!oga_model_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "OGA model is null");
  }

  return *oga_model_;
}

Preprocessor& GenAIModelInstance::GetPreprocessor() {
  if (!preprocessor_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "preprocessor is null");
  }

  return *preprocessor_;
}

const GenAIModelInstance::TagInfo& GenAIModelInstance::GetTagInfo() {
  std::call_once(tag_info_init_flag_, [this]() {
    std::unique_ptr<OgaTokenizer> tokenizer;
    std::unique_ptr<OgaTokenizer> tokenizer_with_special;
    try {
      tokenizer = OgaTokenizer::Create(GetOgaModel());
      tokenizer_with_special = OgaTokenizer::Create(GetOgaModel());
      KeyValuePairs options;
      options.Add("skip_special_tokens", "0");
      tokenizer_with_special->UpdateOptions(options.Keys().data(), options.Values().data(), options.size());
    } catch (...) {
      return;
    }

    // Get tag IDs from the tokenizer (reads from config, with fallback vocab lookup).
    // These throw if the model doesn't define the token, so we catch and leave as nullopt.
    auto try_get_id = [](auto&& getter) -> std::optional<int32_t> {
      try {
        return getter();
      } catch (...) {
        return std::nullopt;
      }
    };
    tag_info_.bot_id = try_get_id([&] { return tokenizer->GetBotTokenId(); });
    tag_info_.eot_id = try_get_id([&] { return tokenizer->GetEotTokenId(); });
    tag_info_.bor_id = try_get_id([&] { return tokenizer->GetBorTokenId(); });
    tag_info_.eor_id = try_get_id([&] { return tokenizer->GetEorTokenId(); });

    // Decode each valid ID once through the special tokenizer to get the string.
    // Uses tokenizer_with_special_ so that special token text (e.g., "<tool_call>") is produced.
    auto decode_id = [&](std::optional<int32_t> id) -> std::string {
      if (!id.has_value()) return {};
      int32_t val = *id;
      OgaString text = tokenizer_with_special->Decode(&val, 1);
      const char* p = text;
      return p ? std::string(p) : std::string();
    };

    tag_info_.bot_str = decode_id(tag_info_.bot_id);
    tag_info_.eot_str = decode_id(tag_info_.eot_id);
    tag_info_.bor_str = decode_id(tag_info_.bor_id);
    tag_info_.eor_str = decode_id(tag_info_.eor_id);
  });

  return tag_info_;
}

std::vector<int32_t> GenAIModelInstance::EncodeText(const std::string& text) {
  try {
    auto sequences = GetPreprocessor().Encode(text.c_str());
    if (!sequences || sequences->Count() == 0) {
      return {};
    }

    const auto* data = sequences->SequenceData(0);
    const auto count = sequences->SequenceCount(0);
    if (data == nullptr || count == 0) {
      return {};
    }

    return {data, data + count};
  } catch (...) {
    return {};
  }
}

}  // namespace fl
