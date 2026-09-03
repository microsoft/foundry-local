// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/model_load_manager.h"

#include "exception.h"
#include "inferencing/generative/genai_config.h"
#include "inferencing/generative/genai_model_instance.h"
#include "platform/path.h"
#include "utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <thread>

namespace fl {

namespace {

/// The expected config filename inside a model directory.
constexpr const char* kGenAIConfigFileName = "genai_config.json";

std::string NormalizeModelPath(std::string_view path) {
  std::filesystem::path normalized;
  std::string error_message;
  if (!platform::GetWeaklyCanonicalPath(std::filesystem::path(std::string(path)), normalized, error_message)) {
    std::error_code ec;
    normalized = std::filesystem::absolute(std::filesystem::path(std::string(path)), ec);
    if (ec) {
      normalized = std::filesystem::path(std::string(path));
    }
  }

  return normalized.lexically_normal().string();
}

bool ModelPathsEqual(std::string_view left, std::string_view right) {
  const auto normalized_left = NormalizeModelPath(left);
  const auto normalized_right = NormalizeModelPath(right);
#ifdef _WIN32
  return normalized_left.size() == normalized_right.size() &&
         std::equal(normalized_left.begin(), normalized_left.end(), normalized_right.begin(), [](char lhs, char rhs) {
           return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
         });
#else
  return normalized_left == normalized_right;
#endif
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

/// Returns whether the provider is DirectML, which is supplied by WinML rather than a downloadable EP bootstrapper.
bool IsDmlProvider(std::string_view provider) {
  return provider == "dml" || provider == "DML" || provider == "DmlExecutionProvider" ||
         provider == "DMLExecutionProvider";
}

/// Returns the required EP registration name for a provider declared in genai_config.json.
std::string RequiredEpForConfigProvider(std::string_view provider) {
  if (provider.empty() || IsDmlProvider(provider)) {
    return {};
  }

  auto ep = EPUtils::StringtoEP(provider);
  if (provider == "WebGpuExecutionProvider") {
    ep = ExecutionProvider::kWebGPU;
  }

  if (ep == ExecutionProvider::kCPU) {
    return {};
  }
  if (ep != ExecutionProvider::kUnknown) {
    return std::string(EPUtils::EPtoRegistrationName(ep));
  }

  // Configs may name WinML/OGA providers that are intentionally outside the public override enum, such as
  // MIGraphXExecutionProvider and RyzenAILightExecutionProvider. Canonical registration names can still be guarded.
  constexpr std::string_view suffix = "ExecutionProvider";
  return provider.ends_with(suffix) ? std::string(provider) : std::string{};
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
                                                         ExecutionProvider ep_override) {
  if (shutdown_.load()) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                     "cannot load model during shutdown");
  }

  const auto path_str = NormalizeModelPath(model_path);
  std::string id_str(model_id);
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = loaded_models_.find(id_str);
  if (it != loaded_models_.end()) {
    if (!ModelPathsEqual(path_str, it->second->ModelPath())) {
      FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model '", id_str,
                       "' is already loaded from a different path: ", it->second->ModelPath(),
                       "; requested path: ", path_str);
    }

    return {LoadStatus::kModelAlreadyLoaded, it->second.get()};
  }

  // Validate model directory exists
  if (!std::filesystem::exists(path_str) || !std::filesystem::is_directory(path_str)) {
    logger_.Log(LogLevel::Error, fmt::format("model path does not exist: {}", path_str));
    return {LoadStatus::kModelNotFound, nullptr};
  }

  logger_.Log(LogLevel::Debug, fmt::format("loading model from {}", path_str));

  // The caller provides the effective model path — the directory containing genai_config.json.
  // DownloadManager and ScanLocalModels resolve this before passing it here.
  auto config_path = (std::filesystem::path(path_str) / kGenAIConfigFileName).string();

  if (!std::filesystem::exists(config_path)) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INTERNAL,
                     "model does not contain ", kGenAIConfigFileName, ": ", id_str);
  }

  auto genai_config = GenAIConfig::LoadFromFile(config_path);

  // Determine execution provider
  auto resolved_ep = ep_override;

  if (resolved_ep == ExecutionProvider::kUnknown) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unknown execution provider override");
  }

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

  std::string required_ep;
  if (resolved_ep == ExecutionProvider::kCPU) {
    // Explicit CPU overrides both the provider in genai_config.json and model-ID hints.
  } else if (resolved_ep != ExecutionProvider::kDefault) {
    required_ep = EPUtils::EPtoRegistrationName(resolved_ep);
  } else {
    const auto config_provider = genai_config.DefaultProvider();
    required_ep = config_provider.empty() ? std::string(RequiredEpForModelId(id_str))
                                          : RequiredEpForConfigProvider(config_provider);
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
  return UnloadModel(model_id, static_cast<const std::string*>(nullptr));
}

bool ModelLoadManager::UnloadModel(std::string_view model_id, std::string_view model_path) {
  const auto normalized_model_path = NormalizeModelPath(model_path);
  return UnloadModel(model_id, &normalized_model_path);
}

bool ModelLoadManager::UnloadModel(std::string_view model_id, const std::string* normalized_model_path) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::string id_str(model_id);
  auto it = loaded_models_.find(id_str);
  if (it == loaded_models_.end()) {
    logger_.Log(LogLevel::Information, fmt::format("model was not loaded: {}", id_str));
    return false;
  }

  if (normalized_model_path && !ModelPathsEqual(*normalized_model_path, it->second->ModelPath())) {
    logger_.Log(LogLevel::Information,
                fmt::format("model '{}' is loaded from a different path", id_str));
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
                  fmt::format("Shutdown: model '{}' still has {} session(s) after overall {}ms deadline; "
                              "leaving loaded",
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
  return GetLoadedModel(model_id, static_cast<const std::string*>(nullptr));
}

GenAIModelInstance* ModelLoadManager::GetLoadedModel(std::string_view model_id, std::string_view model_path) {
  const auto normalized_model_path = NormalizeModelPath(model_path);
  return GetLoadedModel(model_id, &normalized_model_path);
}

GenAIModelInstance* ModelLoadManager::GetLoadedModel(std::string_view model_id,
                                                     const std::string* normalized_model_path) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::string id_str(model_id);
  auto it = loaded_models_.find(id_str);
  if (it != loaded_models_.end() &&
      (!normalized_model_path || ModelPathsEqual(*normalized_model_path, it->second->ModelPath()))) {
    return it->second.get();
  }

  return nullptr;
}

}  // namespace fl
