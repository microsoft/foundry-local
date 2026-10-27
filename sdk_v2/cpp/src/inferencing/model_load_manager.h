// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/execution_provider.h"
#include "inferencing/generative/genai_model_instance.h"
#include "logger.h"
#include "ep_detection/ep_detector.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

class SessionManager;

/// Manages the lifecycle of loaded ORT GenAI models.
/// Thread-safe. Owns all GenAIModelInstance instances.
///
/// Raw pointers are only for immediate queries or callers that already own a live session.
/// AcquireForSession pins an instance across construction so manual or automatic unloading cannot race admission.
class ModelLoadManager {
 public:
  enum class LoadStatus {
    kSuccess,
    kModelNotFound,
    kModelAlreadyLoaded,
  };

  struct LoadResult {
    LoadStatus status;
    GenAIModelInstance* model = nullptr;  // non-owning pointer; lifetime managed by this class
  };

  ModelLoadManager(IEpDetector& ep_detector, ILogger& logger);
  ~ModelLoadManager();

  ModelLoadManager(const ModelLoadManager&) = delete;
  ModelLoadManager& operator=(const ModelLoadManager&) = delete;

  /// Load a model from the given path using ORT GenAI.
  /// @param model_path  Path to the model directory (must contain genai_config.json).
  /// @param model_id    Unique identifier for the model.
  /// @param ep_override Execution provider override (kDefault = use genai_config.json default,
  ///                    or auto-select CUDA for generic-gpu models if available).
  /// @returns LoadResult with status and non-owning pointer to the loaded model.
  LoadResult LoadModel(std::string_view model_path,
                       std::string_view model_id,
                       ExecutionProvider ep_override = ExecutionProvider::kDefault);

  /// Unload a previously loaded model.
  /// @returns true if the model was found and unloaded; false if the model was not loaded
  ///          (idempotent no-op).
  /// @throws  FoundryLocalException(INVALID_USAGE) if the model still has live sessions.
  ///          Drop the sessions before unloading — using a model while unloading it is a
  ///          contract violation.
  bool UnloadModel(std::string_view model_id);

  /// Unload only when the loaded model's normalized path matches `model_path`.
  /// Returns false when the ID is absent or belongs to a different path.
  bool UnloadModel(std::string_view model_id, std::string_view model_path);

  /// Get a loaded model by ID. Returns nullptr if not loaded.
  /// Use AcquireForSession rather than a raw lookup when constructing a session.
  GenAIModelInstance* GetLoadedModel(std::string_view model_id);

  /// Get a loaded model for an immediate query only when its normalized path matches `model_path`.
  GenAIModelInstance* GetLoadedModel(std::string_view model_id, std::string_view model_path);

  /// Pin an instance while constructing a session. Only idle-evicted models are automatically reloaded.
  std::shared_ptr<GenAIModelInstance> AcquireForSession(std::string_view model_id, std::string_view model_path);

  /// Evict idle GPU session caches and models without interrupting requests or live SDK sessions.
  size_t UnloadIdleModels(std::chrono::seconds timeout, SessionManager& sessions,
                          std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

  /// Reject all future LoadModel calls. Called by Manager::Shutdown().
  /// Idempotent and thread-safe.
  void RejectNewLoads();

  /// Unload every loaded model, waiting for live sessions to finish.
  /// Called by Manager::Shutdown after the session manager has cancelled HTTP-tracked
  /// sessions; this covers direct-API sessions (which only register via the per-model
  /// session refcount on GenAIModelInstance).
  ///
  /// The timeout is an *overall* deadline shared across all models — total drain time is
  /// bounded by `timeout` regardless of how many models are loaded. Models whose sessions
  /// have not been released by the deadline are left loaded and a warning is logged; we
  /// do not block process shutdown indefinitely on a stuck caller.
  void UnloadAll(std::chrono::milliseconds timeout = std::chrono::seconds(10));

 private:
  LoadResult LoadModelLocked(const std::string& model_path, const std::string& model_id, ExecutionProvider ep_override);
  bool HasEP(const std::string& ep_name) const;
  bool UnloadModel(std::string_view model_id, const std::string* normalized_model_path);
  GenAIModelInstance* GetLoadedModel(std::string_view model_id, const std::string* normalized_model_path);

  IEpDetector& ep_detector_;
  ILogger& logger_;
  std::atomic<bool> shutdown_{false};
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<GenAIModelInstance>> loaded_models_;
  struct IdleModel {
    std::string path;
    ExecutionProvider ep;
  };
  std::map<std::string, IdleModel> idle_models_;
};

}  // namespace fl
