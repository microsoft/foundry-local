// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/model_load_manager.h"

#include "exception.h"
#include "inferencing/generative/genai_config.h"
#include "inferencing/generative/genai_model_instance.h"
#include "util/model_layout.h"
#include "util/path_safety.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <thread>

namespace fl {

namespace {

/// The expected config filename inside a model directory.
constexpr const char* kGenAIConfigFileName = "genai_config.json";

int ConservativePositiveMinimum(int left, int right) {
  if (left <= 0) {
    return right;
  }

  if (right <= 0) {
    return left;
  }

  return std::min(left, right);
}

struct PackageRuntimeConfig {
  GenAIConfig config;
  bool is_multimodal = false;
};

PackageRuntimeConfig LoadPackageRuntimeConfig(const std::filesystem::path& package_root) {
  const auto manifest_path = package_root / "manifest.json";
  std::ifstream manifest_file(manifest_path);
  if (!manifest_file.is_open()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to open model package manifest: " + manifest_path.string());
  }

  nlohmann::json manifest;
  try {
    manifest_file >> manifest;
  } catch (const nlohmann::json::exception& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to parse model package manifest: " + std::string(e.what()));
  }

  if (!manifest.contains("components") || !manifest["components"].is_object() ||
      manifest["components"].size() != 1) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "Foundry Local Stage 1 requires a model package with exactly one inline component");
  }

  const auto component = manifest["components"].begin().value();
  if (!component.is_object() || !component.contains("variants") || !component["variants"].is_object() ||
      component["variants"].empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model package component does not contain any variants");
  }

  PackageRuntimeConfig result;
  bool has_config = false;
  for (const auto& [variant_name, variant] : component["variants"].items()) {
    if (!variant.is_object()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
               "model package variant '" + variant_name + "' must be an object");
    }

    std::filesystem::path variant_directory = variant_name;
    if (variant.contains("variant_directory")) {
      if (!variant["variant_directory"].is_string()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                 "model package variant '" + variant_name + "' has an invalid variant_directory");
      }

      variant_directory = variant["variant_directory"].get<std::string>();
    }

    const auto variant_path = package_root / variant_directory;
    if (!IsPathWithinDirectory(variant_path, package_root)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
               "model package variant '" + variant_name + "' resolves outside the package root");
    }

    auto config = GenAIConfig::LoadFromFile((variant_path / kGenAIConfigFileName).string());
    result.is_multimodal = result.is_multimodal || (config.model && config.model->IsMultiModal());
    if (!has_config) {
      result.config = std::move(config);
      has_config = true;
      continue;
    }

    if (result.config.model && config.model) {
      if (result.config.model->type != config.model->type) {
        result.config.model->type.clear();
      }

      result.config.model->context_length =
          ConservativePositiveMinimum(result.config.model->context_length, config.model->context_length);
    } else {
      result.config.model.reset();
    }

    if (result.config.search && config.search) {
      result.config.search->max_length =
          ConservativePositiveMinimum(result.config.search->max_length, config.search->max_length);
    } else {
      result.config.search.reset();
    }

    if (result.config.hidden_size != config.hidden_size) {
      result.config.hidden_size.reset();
    }
  }

  if (result.config.model && result.config.model->context_length > 0 &&
      (!result.config.search || result.config.search->max_length <= 0)) {
    result.config.search = GenAIConfig::Search{result.config.model->context_length};
  }

  return result;
}

bool IsTaskMultiModal(std::string_view task) {
  return task == "vision-language-chat" || task == "automatic-speech-recognition";
}

/// Maps model_id substrings to their required execution provider registration name.
/// If a model_id contains one of these keys, the corresponding EP must be registered.
struct ModelIdEpRequirement {
  std::string_view model_id_substr;
  std::string_view required_ep;
};

constexpr ModelIdEpRequirement kModelIdEpRequirements[] = {
    {"cuda-gpu", "CUDAExecutionProvider"},
    {"openvino-npu", "OpenVINOExecutionProvider"},
    {"openvino-gpu", "OpenVINOExecutionProvider"},
    {"qnn-npu", "QNNExecutionProvider"},
    {"trtrtx-gpu", "NvTensorRTRTXExecutionProvider"},
    {"vitis-npu", "VitisAIExecutionProvider"},
};

/// Returns the required EP registration name for a model_id, or empty if none required.
std::string_view RequiredEpForModelId(std::string_view model_id) {
  for (const auto& req : kModelIdEpRequirements) {
    if (model_id.find(req.model_id_substr) != std::string_view::npos) {
      return req.required_ep;
    }
  }

  return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

ModelLoadManager::ModelLoadManager(IEpDetector& ep_detector, ILogger& logger)
    : ep_detector_(ep_detector), logger_(logger) {}

ModelLoadManager::~ModelLoadManager() {
  // Destroy all loaded models under the lock.
  std::lock_guard<std::mutex> lock(mutex_);
  loaded_models_.clear();
}

bool ModelLoadManager::HasEP(const std::string& ep_name) const {
  const auto& device_map = ep_detector_.GetAvailableDevicesToEPs();
  for (const auto& [device, eps] : device_map) {
    if (std::find(eps.begin(), eps.end(), ep_name) != eps.end()) {
      return true;
    }
  }

  return false;
}

// ---------------------------------------------------------------------------
// LoadModel
// ---------------------------------------------------------------------------

ModelLoadManager::LoadResult ModelLoadManager::LoadModel(std::string_view model_path,
                                                         std::string_view model_id,
                                                         ExecutionProvider ep_override,
                                                         std::string_view task) {
  if (shutdown_.load()) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                     "cannot load model during shutdown");
  }

  // Convert to std::string for map operations and string concatenation
  std::string path_str(model_path);
  std::string id_str(model_id);
  std::lock_guard<std::mutex> lock(mutex_);

  // Check if model is already loaded
  auto it = loaded_models_.find(id_str);
  if (it != loaded_models_.end()) {
    return {LoadStatus::kModelAlreadyLoaded, it->second.get()};
  }

  // Validate model directory exists
  if (!std::filesystem::exists(path_str) || !std::filesystem::is_directory(path_str)) {
    logger_.Log(LogLevel::Error, fmt::format("model path does not exist: {}", path_str));
    return {LoadStatus::kModelNotFound, nullptr};
  }

  logger_.Log(LogLevel::Debug, fmt::format("loading model from {}", path_str));

  const auto layout = ClassifyModelLayout(path_str);
  if (layout != ModelLayout::FlatModel && layout != ModelLayout::ModelPackage) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INTERNAL, "model has an unsupported or incomplete layout: ", id_str);
  }

  const bool is_model_package = layout == ModelLayout::ModelPackage;
  GenAIConfig genai_config;
  bool package_is_multimodal = false;
  if (is_model_package) {
    auto package_config = LoadPackageRuntimeConfig(path_str);
    genai_config = std::move(package_config.config);
    package_is_multimodal = package_config.is_multimodal;
  } else {
    genai_config = GenAIConfig::LoadFromFile(
        (std::filesystem::path(path_str) / kGenAIConfigFileName).string());
  }

  const bool is_multimodal =
      package_is_multimodal || (genai_config.model && genai_config.model->IsMultiModal()) || IsTaskMultiModal(task);

  // Determine execution provider
  auto resolved_ep = ep_override;

  if (resolved_ep == ExecutionProvider::kDefault) {
    // Auto-select EP for generic-gpu models: DML models are compatible with
    // CUDA and WebGPU, so try those in order when available.
    if (id_str.find("generic-gpu") != std::string::npos) {
      if (HasEP("CUDAExecutionProvider")) {
        resolved_ep = ExecutionProvider::kCUDA;
        logger_.Log(LogLevel::Information, fmt::format("using CUDA EP for model: {}", id_str));
      } else if (HasEP("WebGpuExecutionProvider")) {
        resolved_ep = ExecutionProvider::kWebGPU;
        logger_.Log(LogLevel::Information, fmt::format("using WebGPU EP for model: {}", id_str));
      }
    }
  }

  std::string_view required_ep;
  if (resolved_ep != ExecutionProvider::kDefault && resolved_ep != ExecutionProvider::kCPU) {
    required_ep = EPUtils::EPtoRegistrationName(resolved_ep);
  } else {
    required_ep = RequiredEpForModelId(id_str);
  }

  // OGA can crash or hang if a model is loaded with an unregistered EP.
  if (!required_ep.empty() && !HasEP(std::string(required_ep))) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                     "model '", id_str, "' requires ", required_ep,
                     " which is not registered. Call DownloadAndRegisterEps() first.");
  }

  if (!required_ep.empty() && !ep_detector_.PrepareForModelLoad(required_ep)) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INTERNAL,
                     "failed to prepare ", required_ep, " for model loading");
  }

  // std::make_unique cannot access the private constructor; using new directly is intentional.
  auto loaded = std::unique_ptr<GenAIModelInstance>(new GenAIModelInstance(id_str,
                                                                           path_str,
                                                                           std::move(genai_config),
                                                                           resolved_ep,
                                                                           is_model_package,
                                                                           is_multimodal,
                                                                           logger_));

  auto* raw_ptr = loaded.get();
  loaded_models_[id_str] = std::move(loaded);

  logger_.Log(LogLevel::Information, fmt::format("model loaded successfully: {}", id_str));
  return {LoadStatus::kSuccess, raw_ptr};
}

// ---------------------------------------------------------------------------
// UnloadModel
// ---------------------------------------------------------------------------

bool ModelLoadManager::UnloadModel(std::string_view model_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::string id_str(model_id);
  auto it = loaded_models_.find(id_str);
  if (it == loaded_models_.end()) {
    logger_.Log(LogLevel::Information, fmt::format("model was not loaded: {}", id_str));
    return false;
  }

  // Refuse to unload while sessions hold the instance — the OGA objects must outlive
  // every session that referenced them. Asking to unload an in-use model is a caller
  // contract violation, not a recoverable state.
  auto live_sessions = it->second->SessionRefCount();
  if (live_sessions > 0) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                     "cannot unload model '", id_str, "': ", live_sessions,
                     " session(s) still using it");
  }

  logger_.Log(LogLevel::Information, fmt::format("unloading model: {}", id_str));

  // Erasing destroys the GenAIModelInstance, which destroys OGA objects in reverse order.
  loaded_models_.erase(it);
  return true;
}

void ModelLoadManager::RejectNewLoads() {
  shutdown_.store(true);
}

void ModelLoadManager::UnloadAll(std::chrono::milliseconds timeout) {
  // Snapshot ids+pointers under the lock; the GenAIModelInstance pointers stay valid
  // because (a) only this method or UnloadModel can erase entries, and (b) we serialize
  // the per-id erase below.
  std::vector<std::pair<std::string, GenAIModelInstance*>> snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.reserve(loaded_models_.size());
    for (auto& [id, instance] : loaded_models_) {
      snapshot.emplace_back(id, instance.get());
    }
  }

  if (snapshot.empty()) {
    return;
  }

  logger_.Log(LogLevel::Information,
              fmt::format("Shutdown: unloading {} model(s)", snapshot.size()));

  using clock = std::chrono::steady_clock;
  constexpr auto kPollInterval = std::chrono::milliseconds(50);

  // Overall deadline shared across all models — shutdown must be bounded regardless of
  // how many models are loaded. A stuck caller on one model shouldn't extend total drain
  // time linearly with model count.
  auto deadline = clock::now() + timeout;

  for (auto& [id, instance] : snapshot) {
    while (instance->SessionRefCount() > 0 && clock::now() < deadline) {
      std::this_thread::sleep_for(kPollInterval);
    }

    auto remaining = instance->SessionRefCount();
    if (remaining > 0) {
      logger_.Log(LogLevel::Warning,
                  fmt::format("Shutdown: model '{}' still has {} session(s) after overall {}ms deadline; leaving loaded",
                              id, remaining, timeout.count()));
      continue;
    }

    try {
      UnloadModel(id);
    } catch (const std::exception& ex) {
      // A new session attached between our refcount poll and the lock acquisition inside
      // UnloadModel. Log and move on — IsShutdownRequested-gated callers shouldn't be
      // creating new sessions, but we don't crash shutdown over it.
      logger_.Log(LogLevel::Warning,
                  fmt::format("Shutdown: failed to unload '{}': {}", id, ex.what()));
    }
  }
}

// ---------------------------------------------------------------------------
// GetLoadedModel
// ---------------------------------------------------------------------------

GenAIModelInstance* ModelLoadManager::GetLoadedModel(std::string_view model_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::string id_str(model_id);
  auto it = loaded_models_.find(id_str);
  if (it != loaded_models_.end()) {
    return it->second.get();
  }

  return nullptr;
}

}  // namespace fl
