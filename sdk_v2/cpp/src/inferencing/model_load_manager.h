// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/execution_provider.h"
#include "inferencing/generative/genai_model_instance.h"
#include "inferencing/model_session_lease.h"
#include "logger.h"
#include "ep_detection/ep_detector.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

/// Manages the lifecycle of loaded ORT GenAI models.
/// Thread-safe. Owns all GenAIModelInstance entries through shared_ptr.
///
/// Two ways to reach a loaded model, and only one of them is lifetime-safe:
///
///   - GetLoadedModel() returns a *non-owning* pointer valid only while the model stays loaded. It exists
///     for callers that merely inspect the instance.
///   - AcquireLoadedModel() returns a ModelSessionLease. The shutdown check, the loaded check and the lease
///     increment all happen inside one critical section here, so a model cannot be unloaded between "it is
///     loaded" and "I hold a reference to it". This is what every session runtime uses.
///
/// UnloadModel refuses while any lease is outstanding and always destroys the entry *outside* this manager's
/// mutex, so releasing OGA objects never runs under a lock a concurrent loader could be waiting on.
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
  ~ModelLoadManager() noexcept;

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
  /// The returned pointer is valid until UnloadModel is called for this model_id. Prefer
  /// AcquireLoadedModel() whenever the caller intends to *use* the model.
  GenAIModelInstance* GetLoadedModel(std::string_view model_id);

  /// Take a lease on a loaded model. Returns nullopt while shutting down or when the model is not loaded.
  ///
  /// The check and the increment are one critical section: an UnloadModel racing this call either observes
  /// the lease (and refuses) or completes before the lease is taken (and this returns nullopt). The returned
  /// lease keeps the entry alive on its own, so it stays valid even if this manager is destroyed.
  [[nodiscard]] std::optional<ModelSessionLease> AcquireLoadedModel(std::string_view model_id);

  /// Close load admission and wake same-ID waiters. Non-blocking apart from taking the bookkeeping mutex;
  /// already-admitted model lifecycle work continues until DrainModelLifecycleWork().
  void CloseLoadAdmission();

  /// Wait until every LoadModel call admitted before closure and every already-detached model teardown has
  /// returned. Call CloseLoadAdmission() first. Idempotent and thread-safe.
  void DrainModelLifecycleWork();

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
  class LoadCallGuard;
  class UnloadReservationGuard;

  bool HasEP(const std::string& ep_name) const;
  void ReleaseLoadCall(const std::string& id, bool reserved) noexcept;
  void ReleaseUnloadReservation(const std::string& id) noexcept;

  IEpDetector& ep_detector_;
  ILogger& logger_;
  mutable std::mutex mutex_;

  /// Guarded admission state. active_load_calls_ includes reservation owners and same-ID waiters admitted
  /// before closure, so manager destruction can wait until no LoadModel stack still references manager fields.
  /// active_load_ids_ provides the corresponding per-ID drain used by UnloadModel.
  bool admission_closed_ = false;
  size_t active_load_calls_ = 0;
  std::map<std::string, size_t> active_load_ids_;

  /// IDs currently being loaded. A load reserves its ID under mutex_ before doing the heavy filesystem /
  /// config / EP / model-construction work *outside* the lock, then publishes under mutex_. A concurrent
  /// LoadModel for the same ID waits on load_cv_ until the reservation is released rather than duplicating the
  /// work (or racing to insert two instances for the same ID).
  std::set<std::string> loading_ids_;
  /// IDs detached by UnloadModel whose engine teardown is still running outside mutex_. A same-ID reload
  /// waits until teardown completes rather than constructing a replacement concurrently.
  std::set<std::string> unloading_ids_;
  std::condition_variable load_cv_;

  /// Shared, stable entries. Held by shared_ptr so an outstanding ModelSessionLease keeps the OGA objects
  /// alive independently of this map — and independently of this manager.
  std::map<std::string, std::shared_ptr<GenAIModelInstance>> loaded_models_;
};

}  // namespace fl
