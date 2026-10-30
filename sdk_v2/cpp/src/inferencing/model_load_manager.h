// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/execution_provider.h"
#include "inferencing/generative/genai_model_instance.h"
#include "inferencing/predictive/non_generative_admission.h"
#include "logger.h"
#include "ep_detection/ep_detector.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

/// Manages the lifecycle of loaded ORT GenAI models.
/// Thread-safe. Owns all GenAIModelInstance instances.
///
/// Returned GenAIModelInstance pointers are non-owning and valid only while the
/// model remains loaded. The caller must ensure the model is not unloaded (via
/// UnloadModel) while the pointer is in use. Session factories use AcquireSessionModel
/// to acquire their construction-time reference atomically with lookup.
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

  ModelLoadManager(
      IEpDetector& ep_detector, ILogger& logger,
      uint64_t non_generative_memory_budget_bytes =
          (std::numeric_limits<uint64_t>::max)(),
      size_t non_generative_readiness_concurrency = 1);
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

  /// Verify that a required provider is registered and prepare its runtime
  /// dependencies before constructing an ORT GenAI model or session.
  void PrepareProviderForModelLoad(std::string_view registration_name,
                                   std::string_view model_id);
  std::shared_ptr<NonGenerativeAdmissionCoordinator>
  NonGenerativeAdmission() const {
    return non_generative_admission_;
  }

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
  /// The returned pointer is valid until UnloadModel is called for this model_id.
  GenAIModelInstance* GetLoadedModel(std::string_view model_id);

  /// Get a loaded model for an immediate query only when its normalized path matches `model_path`.
  GenAIModelInstance* GetLoadedModel(std::string_view model_id, std::string_view model_path);

  struct SessionReferenceDeleter {
    void operator()(GenAIModelInstance* model) const noexcept { model->ReleaseSession(); }
  };
  using SessionModelReference = std::unique_ptr<GenAIModelInstance, SessionReferenceDeleter>;

  /// Acquire the construction-time session reference under the lifecycle lock. The session factory retains it until
  /// the constructed session has acquired its own reference, or releases it on construction failure.
  SessionModelReference AcquireSessionModel(std::string_view model_id, std::string_view model_path);

  struct LoadedModelState {
    GenAIConfig config;
    std::string model_path;
    std::optional<uint64_t> context_length;
  };

  /// Copy loaded configuration, actual path, and effective context under the lock, qualified by ID and artifact path.
  /// Engine limits come from its already-initialized native capabilities, without creating a request or session.
  std::optional<LoadedModelState> GetLoadedModelState(std::string_view model_id, std::string_view model_path);

  /// Reject operations on a directory containing any loaded model, regardless of ID or revision.
  /// Holds the lifecycle mutex through the operation so another handle cannot load files being removed.
  /// The operation must not call back into this ModelLoadManager.
  void RunWithUnloadedModelsInDirectory(std::string_view directory, const std::function<void()>& operation);

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
  bool UnloadModel(std::string_view model_id, const std::string* normalized_model_path);
  GenAIModelInstance* GetLoadedModel(std::string_view model_id, const std::string* normalized_model_path);
  GenAIModelInstance* FindLoadedModelLocked(std::string_view model_id, const std::string* normalized_model_path);

  IEpDetector& ep_detector_;
  ILogger& logger_;
  std::shared_ptr<NonGenerativeAdmissionCoordinator>
      non_generative_admission_;
  std::atomic<bool> shutdown_{false};
  mutable std::mutex mutex_;
  std::map<std::string, std::unique_ptr<GenAIModelInstance>> loaded_models_;
};

}  // namespace fl
