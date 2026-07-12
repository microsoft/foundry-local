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

/// Manages the lifecycle of loaded ORT GenAI models.
/// Thread-safe. Owns all GenAIModelInstance instances.
///
/// Returned GenAIModelInstance pointers are non-owning and valid only while the
/// model remains loaded. The caller must ensure the model is not unloaded (via
/// UnloadModel) while the pointer is in use. In practice, the Manager serializes
/// load/unload operations through the web service, so concurrent unload during
/// active inference does not occur.
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

  class LoadedModelLease {
   public:
    LoadedModelLease() = default;
    ~LoadedModelLease();

    LoadedModelLease(const LoadedModelLease&) = delete;
    LoadedModelLease& operator=(const LoadedModelLease&) = delete;

    LoadedModelLease(LoadedModelLease&& other) noexcept;
    LoadedModelLease& operator=(LoadedModelLease&& other) noexcept;

    explicit operator bool() const { return model_ != nullptr; }
    GenAIModelInstance* get() const { return model_; }
    GenAIModelInstance& operator*() const { return *model_; }
    GenAIModelInstance* operator->() const { return model_; }

    void Reset() noexcept;
    void Release() noexcept { model_ = nullptr; }

   private:
    explicit LoadedModelLease(GenAIModelInstance* model) : model_(model) {}

    GenAIModelInstance* model_ = nullptr;

    friend class ModelLoadManager;
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

  /// Get a loaded model by ID. Returns nullptr if not loaded.
  /// The returned pointer is valid until UnloadModel is called for this model_id.
  GenAIModelInstance* GetLoadedModel(std::string_view model_id);

  /// Get a loaded model by ID and increment its live-session refcount while
  /// holding the load-manager lock. Returns an empty lease if not loaded.
  LoadedModelLease AcquireLoadedModel(std::string_view model_id);

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
  bool HasEP(const std::string& ep_name) const;

  IEpDetector& ep_detector_;
  ILogger& logger_;
  std::atomic<bool> shutdown_{false};
  mutable std::mutex mutex_;
  std::map<std::string, std::unique_ptr<GenAIModelInstance>> loaded_models_;
};

}  // namespace fl
