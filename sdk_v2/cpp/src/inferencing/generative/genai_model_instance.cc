// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/genai_model_instance.h"
#include "exception.h"
#include "inferencing/execution_provider.h"
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

  try {
    preprocessor_ = Preprocessor::Create(*oga_model_, IsMultiModal());
  } catch (const std::runtime_error& e) {
    FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL,
                     "failed to create preprocessor for model ", model_id_, ": ", e.what());
  }
}

// Destructor: unique_ptr members are destroyed in reverse declaration order.
// OGA objects have custom operator delete that calls OgaDestroy* functions.
// Destruction order: preprocessor → oga_model (correct: dependents first).
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

}  // namespace fl
