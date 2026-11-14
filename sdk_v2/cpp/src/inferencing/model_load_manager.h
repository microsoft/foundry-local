// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/execution_provider.h"
#include "inferencing/generative/genai_model_instance.h"
#include "inferencing/predictive/non_generative_admission.h"
#include "catalog/non_generative_package.h"
#include "logger.h"
#include "ep_detection/ep_detector.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

class Model;
struct ModelInfo;
class ModelLoadManager;
class ManagedModelRuntime;
class ModelLoadManagerTestAccess;

/// Keeps an admitted hosted load's selection stable until it succeeds or unwinds. The manager must outlive the lease.
class LoadedSelectionReservation {
 public:
  ~LoadedSelectionReservation();
  LoadedSelectionReservation(const LoadedSelectionReservation&) = delete;
  LoadedSelectionReservation& operator=(const LoadedSelectionReservation&) = delete;
  void Commit() noexcept;

 private:
  friend class ModelLoadManager;
  LoadedSelectionReservation(ModelLoadManager& manager, std::pair<std::string, std::string> identity);
  ModelLoadManager& manager_;
  const std::pair<std::string, std::string> identity_;
  bool active_ = false;
};

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

  class PredictiveRequestLease {
   public:
    PredictiveRequestLease(PredictiveRequestLease&& other) noexcept;
    PredictiveRequestLease& operator=(PredictiveRequestLease&&) = delete;
    ~PredictiveRequestLease();

    PredictiveRequestLease(const PredictiveRequestLease&) = delete;
    PredictiveRequestLease& operator=(const PredictiveRequestLease&) = delete;

   private:
    friend class ModelLoadManager;
    explicit PredictiveRequestLease(ModelLoadManager& manager)
        : manager_(&manager) {}
    ModelLoadManager* manager_;
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
  ///                    or auto-select CUDA for text generic-gpu models and WebGPU for multimodal ones if available).
  /// @returns LoadResult with status and non-owning pointer to the loaded model.
  LoadResult LoadModel(std::string_view model_path,
                       std::string_view model_id,
                       ExecutionProvider ep_override = ExecutionProvider::kDefault,
                       const ModelInfo* model_info = nullptr);

  /// Verify that a required provider is registered and prepare its runtime
  /// dependencies before constructing an ORT GenAI model or session.
  void PrepareProviderForModelLoad(std::string_view registration_name,
                                   std::string_view model_id);
  std::shared_ptr<void> AcquireNonGenerativeRuntime(
      Model& model,
      const std::function<void()>& on_admitted = {});
  std::shared_ptr<void> AcquireLoadedNonGenerativeRuntime(Model& model);
  bool UnloadNonGenerativeRuntime(Model& model);
  bool IsModelLoaded(const Model& model) const;
  bool IsModelLoading(const Model& model) const;
  bool IsModelArtifactLoaded(std::string_view model_id,
                             std::string_view model_path) const;
  bool UnloadModelArtifact(std::string_view model_id,
                           std::string_view model_path);
  void RegisterPredictiveSession(void* identity,
                                 std::function<void()> cancel,
                                 std::function<void()> release);
  void UnregisterPredictiveSession(void* identity) noexcept;
  PredictiveRequestLease AcquirePredictiveRequest();
  void WaitForPredictiveRequests();
  void ReleasePredictiveSessions();

  /// Unload a previously loaded model.
  /// @returns true if the model was found and unloaded; false if the model was not loaded
  ///          (idempotent no-op).
  /// @throws  FoundryLocalException(INVALID_USAGE) if the model still has live sessions.
  ///          Drop the sessions before unloading — using a model while unloading it is a
  ///          contract violation.
  ///          An ID shared by multiple loaded artifacts requires the path overload.
  bool UnloadModel(std::string_view model_id);

  /// Unload only when the loaded model's normalized path matches `model_path`.
  /// Returns false when the ID is absent or belongs to a different path.
  bool UnloadModel(std::string_view model_id, std::string_view model_path);

  /// Get a loaded model by ID. Returns nullptr if not loaded; throws if multiple artifacts share the ID.
  /// The returned pointer is valid until UnloadModel is called for this model_id.
  GenAIModelInstance* GetLoadedModel(std::string_view model_id);

  /// Get a loaded model for an immediate query only when its normalized path matches `model_path`.
  GenAIModelInstance* GetLoadedModel(std::string_view model_id, std::string_view model_path);

  /// Preserve hosted selection by artifact identity without reporting locally loaded weights.
  /// Returns whether that artifact already had a committed pin.
  bool PreserveLoadedSelection(std::string_view model_id, std::string_view model_path, bool preserve);
  bool HasPreservedLoadedSelection(std::string_view model_id, std::string_view model_path) const;
  std::unique_ptr<LoadedSelectionReservation> ReserveLoadedSelection(std::string_view model_id,
                                                                     std::string_view model_path);
  /// Recheck committed hosted pins without holding the manager lock across remote calls.
  void ReconcileLoadedSelections(const std::function<bool(const std::string&, const std::string&)>& is_loaded);

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
  friend class LoadedSelectionReservation;
  friend class ModelLoadManagerTestAccess;
  void CompleteSelectionReservation(const std::pair<std::string, std::string>& identity, bool commit) noexcept;
  std::shared_ptr<void> AcquireManagedRuntime(
      std::string identity, std::string model_id, std::string model_path,
      uint64_t estimated_resident_bytes, std::string readiness_group,
      const std::function<std::shared_ptr<void>()>& factory,
      const std::function<void()>& on_admitted = {},
      std::shared_ptr<const GenAIConfig> config = {});

  bool HasEP(const std::string& ep_name) const;
  void ReleasePredictiveRequest() noexcept;
  NonGenerativePackageMetadata InspectNonGenerativePackage(Model& model);
  bool UnloadModel(std::string_view model_id, const std::string* normalized_model_path);
  GenAIModelInstance* GetLoadedModel(std::string_view model_id, const std::string* normalized_model_path);
  GenAIModelInstance* FindLoadedModelLocked(std::string_view model_id, const std::string* normalized_model_path);

  IEpDetector& ep_detector_;
  ILogger& logger_;
  std::shared_ptr<NonGenerativeAdmissionCoordinator>
      non_generative_admission_;
  std::atomic<bool> shutdown_{false};
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<ManagedModelRuntime>> loaded_runtimes_;
  struct LoadingRuntimeEntry {
    std::string model_id;
    std::string model_path;
    std::shared_future<std::shared_ptr<ManagedModelRuntime>> runtime;
  };
  std::map<std::string, LoadingRuntimeEntry> loading_runtimes_;
  std::mutex predictive_mutex_;
  std::condition_variable predictive_condition_;
  struct PredictiveSessionCallbacks {
    std::function<void()> cancel;
    std::function<void()> release;
  };
  std::map<void*, PredictiveSessionCallbacks> predictive_sessions_;
  size_t active_predictive_requests_{};
  std::map<std::pair<std::string, std::string>,
           NonGenerativePackageMetadata>
      non_generative_packages_;
  struct SelectionState {
    bool preserved = false;
    size_t reservations = 0;
    uint64_t generation = 0;
  };
  std::map<std::pair<std::string, std::string>, SelectionState> preserved_selections_;
  uint64_t selection_generation_ = 0;
};

}  // namespace fl
