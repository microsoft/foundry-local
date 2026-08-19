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
                                       bool is_model_package,
                                       bool is_multimodal,
                                       ILogger& logger)
    : model_id_(std::move(model_id)),
      model_path_(std::move(effective_model_path)),
      genai_config_(std::move(genai_config)),
      ep_(resolved_ep),
      is_model_package_(is_model_package),
      is_multimodal_(is_multimodal),
      last_activity_(std::chrono::steady_clock::now()) {
  std::unique_ptr<OgaConfig> oga_config;
  try {
    if (is_model_package_) {
      const auto provider = EPUtils::EPtoRegistrationName(ep_);
      oga_config = OgaConfig::CreateFromPackageEp(model_path_.c_str(), provider.empty() ? nullptr : provider.data());
    } else {
      oga_config = OgaConfig::Create(model_path_.c_str());
    }
  } catch (const std::runtime_error& e) {
    FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL,
                     "failed to create OGA config for model ", model_id_, ": ", e.what());
  }

  // Package variant selection consumes the EP while creating the config. Mutating providers afterward would not
  // reselect the package variant and could run a compiled graph with the wrong EP.
  // kCPU is excluded because EPtoGenAI returns "" for it; an empty provider list already means CPU.
  if (!is_model_package_ && ep_ != ExecutionProvider::kDefault && ep_ != ExecutionProvider::kCPU) {
    try {
      oga_config->ClearProviders();
      std::string_view provider_str = EPUtils::EPtoGenAI(ep_);
      oga_config->AppendProvider(provider_str.data());
    } catch (const std::runtime_error& e) {
      FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL,
                       "failed to configure EP for model ", model_id_, ": ", e.what());
    }
  }

  if (ep_ == ExecutionProvider::kCUDA) {
    try {
      // Provider options may be updated after package selection; only changing the provider list would invalidate it.
      oga_config->SetProviderOption("cuda", "enable_cuda_graph", "0");
    } catch (const std::runtime_error& e) {
      FL_LOG_AND_THROW(logger, FOUNDRY_LOCAL_ERROR_INTERNAL,
                       "failed to configure CUDA options for model ", model_id_, ": ", e.what());
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
  return is_multimodal_;
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
