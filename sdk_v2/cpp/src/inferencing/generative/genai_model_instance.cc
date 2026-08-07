// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/genai_model_instance.h"
#include "exception.h"
#include "inferencing/execution_provider.h"
#include "util/key_value_pairs.h"
#include "utils.h"

#include <ort_genai.h>

#include <fmt/format.h>

namespace fl {

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
  } catch (const std::runtime_error& e) {
    FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL,
                     "failed to load model ", model_id_, ": ", e.what());
  }

  // Create Tokenizer
  try {
    tokenizer_ = std::make_unique<fl::Tokenizer>(OgaTokenizer::Create(*oga_model_));
  } catch (const std::runtime_error& e) {
    FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL,
                     "failed to create tokenizer for model ", model_id_, ": ", e.what());
  }

  // Create second Tokenizer for special token detection
  try {
    tokenizer_with_special_ = OgaTokenizer::Create(*oga_model_);
    KeyValuePairs options;
    options.Add("skip_special_tokens", "0");
    tokenizer_with_special_->UpdateOptions(options.Keys().data(), options.Values().data(), options.size());
  } catch (const std::runtime_error& e) {
    FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL,
                     "failed to create special-token tokenizer for model ", model_id_, ": ", e.what());
  }

  // Create MultiModalProcessor if multimodal
  if (genai_config_.model.has_value() && genai_config_.model->IsMultiModal()) {
    try {
      processor_ = OgaMultiModalProcessor::Create(*oga_model_);
    } catch (const std::runtime_error& e) {
      FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL,
                       "failed to create multimodal processor for model ", model_id_, ": ", e.what());
    }
  }
}

// Destructor: unique_ptr members are destroyed in reverse declaration order.
// OGA objects have custom operator delete that calls OgaDestroy* functions.
// Destruction order: processor → tokenizer → oga_model (correct: dependents first).
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

OgaTokenizer& GenAIModelInstance::GetOgaTokenizerWithSpecial() {
  if (!tokenizer_with_special_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "OGA tokenizer with special is null");
  }

  return *tokenizer_with_special_;
}

Tokenizer& GenAIModelInstance::Tokenizer() {
  if (!tokenizer_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "OGA tokenizer is null");
  }

  return *tokenizer_;
}

OgaMultiModalProcessor* GenAIModelInstance::GetProcessor() {
  return processor_.get();
}

const std::vector<int32_t>& GenAIModelInstance::GetEosTokenIds() {
  std::call_once(eos_token_ids_init_flag_, [this]() {
    auto ids = tokenizer_->Oga().GetEosTokenIds();
    eos_token_ids_.assign(ids.begin(), ids.end());
  });

  return eos_token_ids_;
}

}  // namespace fl
