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

constexpr std::string_view kExpectedToolCallBlock =
    "<tool_call>\n"
    "<function=probe_function>\n"
    "<parameter=probe_parameter>\n"
    "probe_value\n"
    "</parameter>\n"
    "</function>\n"
    "</tool_call>";
constexpr std::string_view kFirstToolCallBlock =
    "<tool_call>\n<function=probe_first>\n</function>\n</tool_call>";
constexpr std::string_view kSecondToolCallBlock =
    "<tool_call>\n<function=probe_second>\n</function>\n</tool_call>";
constexpr std::string_view kFirstToolResultBlock =
    "<tool_response>\nprobe-result-first\n</tool_response>";
constexpr std::string_view kSecondToolResultBlock =
    "<tool_response>\nprobe-result-second\n</tool_response>";

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

ModelCapabilities ResolveModelCapabilities(std::string_view model_type, Preprocessor& preprocessor) noexcept {
  if (model_type != kQwen35TextModelType) {
    return {};
  }

  try {
    const auto tool_call_projection =
        preprocessor.ApplyChatTemplate(kToolCallProbeMessages, /*tools_json=*/nullptr, /*add_generation_prompt=*/false);
    const auto tool_result_projection = preprocessor.ApplyChatTemplate(
        kToolResultProbeMessages, /*tools_json=*/nullptr, /*add_generation_prompt=*/false);
    return model_capabilities_internal::ResolveRenderedProbes(
        model_type, tool_call_projection, tool_result_projection);
  } catch (...) {
    return {};
  }
}

}  // namespace

ModelCapabilities model_capabilities_internal::ResolveRenderedProbes(
    std::string_view model_type,
    std::string_view tool_call_projection,
    std::string_view tool_result_projection) noexcept {
  if (model_type != kQwen35TextModelType) {
    return {};
  }

  const bool native_qwen_xml_tool_calls =
      tool_call_projection.find(kExpectedToolCallBlock) != std::string_view::npos;
  const bool positional_tool_results =
      ContainsInOrder(tool_result_projection,
                      {kFirstToolCallBlock, kSecondToolCallBlock, kFirstToolResultBlock,
                       kSecondToolResultBlock}) &&
      tool_result_projection.find("probe-call-a") == std::string_view::npos &&
      tool_result_projection.find("probe-call-b") == std::string_view::npos;

  return {native_qwen_xml_tool_calls, positional_tool_results};
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
      last_activity_(std::chrono::steady_clock::now()) {
  // Create OGA Config from the effective model directory
  std::unique_ptr<OgaConfig> oga_config;
  try {
    oga_config = OgaConfig::Create(model_path_.c_str());
  } catch (const std::runtime_error& e) {
    FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL,
                     "failed to create OGA config for model ", model_id_, ": ", e.what());
  }

  // Every explicit EP overrides providers from genai_config.json. CPU is OGA's default when the provider list is
  // empty, and EPtoGenAI intentionally has no CPU name, so CPU clears the list without appending a provider.
  if (ep_ != ExecutionProvider::kDefault) {
    try {
      oga_config->ClearProviders();
      if (ep_ != ExecutionProvider::kCPU) {
        std::string_view provider_str = EPUtils::EPtoGenAI(ep_);
        oga_config->AppendProvider(provider_str.data());
      }

      // Disable CUDA graph for CUDA EP (matches C# behavior)
      if (ep_ == ExecutionProvider::kCUDA) {
        oga_config->SetProviderOption("cuda", "enable_cuda_graph", "0");
      }
    } catch (const std::runtime_error& e) {
      FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL,
                       "failed to configure EP for model ", model_id_, ": ", e.what());
    }
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

  capabilities_ = ResolveModelCapabilities(model_type_, *preprocessor_);

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
