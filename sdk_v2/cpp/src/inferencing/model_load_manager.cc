// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/model_load_manager.h"

#include "exception.h"
#include "inferencing/generative/genai_config.h"
#include "inferencing/generative/genai_model_instance.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <utility>

namespace fl {

namespace {

/// The expected config filename inside a model directory.
constexpr const char* kGenAIConfigFileName = "genai_config.json";

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

#ifdef _WIN32
/// SetDllDirectoryW is process-global. Every ModelLoadManager instance shares this lock so EP preparation
/// cannot redirect another concurrent OGA model construction. The manager map mutex is never held here.
std::unique_lock<std::mutex> AcquireModelEpLifecycleLock() {
  static std::mutex lifecycle_mutex;
  return std::unique_lock<std::mutex>(lifecycle_mutex);
}
#else
/// Model/EP construction does not mutate a process-global loader path on POSIX, so it remains parallel.
struct ModelEpLifecycleLock {};

ModelEpLifecycleLock AcquireModelEpLifecycleLock() {
  return {};
}
#endif

}  // namespace

class ModelLoadManager::LoadCallGuard {
 public:
  LoadCallGuard(ModelLoadManager& manager, const std::string& id) : manager_(manager), id_(id) {
    std::lock_guard<std::mutex> lock(manager_.mutex_);
    if (!manager_.admission_closed_) {
      ++manager_.active_load_ids_[id_];
      ++manager_.active_load_calls_;
      admitted_ = true;
    }
  }

  ~LoadCallGuard() {
    if (admitted_) {
      manager_.ReleaseLoadCall(id_, reserved_);
    }
  }

  LoadCallGuard(const LoadCallGuard&) = delete;
  LoadCallGuard& operator=(const LoadCallGuard&) = delete;

  bool IsAdmitted() const noexcept { return admitted_; }
  void MarkReserved() noexcept { reserved_ = true; }

 private:
  ModelLoadManager& manager_;
  const std::string& id_;
  bool admitted_ = false;
  bool reserved_ = false;
};

class ModelLoadManager::UnloadReservationGuard {
 public:
  UnloadReservationGuard() = default;

  ~UnloadReservationGuard() {
    if (manager_ != nullptr) {
      manager_->ReleaseUnloadReservation(*id_);
    }
  }

  UnloadReservationGuard(const UnloadReservationGuard&) = delete;
  UnloadReservationGuard& operator=(const UnloadReservationGuard&) = delete;

  void Engage(ModelLoadManager& manager, const std::string& id) noexcept {
    manager_ = &manager;
    id_ = &id;
  }

 private:
  ModelLoadManager* manager_ = nullptr;
  const std::string* id_ = nullptr;
};

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

ModelLoadManager::ModelLoadManager(IEpDetector& ep_detector, ILogger& logger)
    : ep_detector_(ep_detector), logger_(logger) {}

ModelLoadManager::~ModelLoadManager() noexcept {
  // Load and unload-reservation guards reference this manager's mutex/CV/sets. Closing and draining first
  // guarantees every guard has released those references before member destruction begins, including during
  // partial Manager teardown.
  CloseLoadAdmission();
  DrainModelLifecycleWork();

  // Detach the entries under the lock, destroy them outside it. Releasing OGA objects is slow and can call
  // back into logging; it must never run with this manager's mutex held. Any entry still referenced by an
  // outstanding ModelSessionLease simply survives here and dies with that lease.
  std::map<std::string, std::shared_ptr<GenAIModelInstance>> detached;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    detached.swap(loaded_models_);
  }
}

void ModelLoadManager::ReleaseLoadCall(const std::string& id, bool reserved) noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (reserved) {
      loading_ids_.erase(id);
    }

    if (auto it = active_load_ids_.find(id); it != active_load_ids_.end()) {
      if (it->second > 1) {
        --it->second;
      } else {
        active_load_ids_.erase(it);
      }
    }

    if (active_load_calls_ > 0) {
      --active_load_calls_;
    }
  }

  load_cv_.notify_all();
}

void ModelLoadManager::ReleaseUnloadReservation(const std::string& id) noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    unloading_ids_.erase(id);
  }

  load_cv_.notify_all();
}

void ModelLoadManager::CloseLoadAdmission() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    admission_closed_ = true;
  }

  // Wake same-ID waiters so they observe closure and release their admitted-call guards.
  load_cv_.notify_all();
}

void ModelLoadManager::DrainModelLifecycleWork() {
  std::unique_lock<std::mutex> lock(mutex_);
  load_cv_.wait(lock, [this] {
    return active_load_calls_ == 0 && unloading_ids_.empty();
  });
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
  // Convert to std::string for map operations and string concatenation
  std::string path_str(model_path);
  std::string id_str(model_id);

  LoadCallGuard load_call(*this, id_str);
  if (!load_call.IsAdmitted()) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot load model during shutdown");
  }

  // Phase 1 — reserve under the lock. Re-check shutdown and the loaded map on every wakeup: a concurrent load
  // of the same ID either publishes it (we then return kModelAlreadyLoaded) or fails and releases its
  // reservation (we then take the load over ourselves). No filesystem, config, EP or model-construction work
  // happens here — the lock only bookkeeps the reservation.
  GenAIModelInstance* existing_model = nullptr;
  bool shutdown_rejected = false;

  {
    std::unique_lock<std::mutex> lock(mutex_);

    load_cv_.wait(lock, [&] {
      return admission_closed_ || loaded_models_.contains(id_str) ||
             (!loading_ids_.contains(id_str) && !unloading_ids_.contains(id_str));
    });

    if (admission_closed_) {
      shutdown_rejected = true;
    } else if (auto it = loaded_models_.find(id_str); it != loaded_models_.end()) {
      existing_model = it->second.get();
    } else {
      loading_ids_.insert(id_str);
      load_call.MarkReserved();
    }
  }

  if (shutdown_rejected) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot load model during shutdown");
  }

  if (existing_model != nullptr) {
    return {LoadStatus::kModelAlreadyLoaded, existing_model};
  }

  // Phase 2 — the heavy work, with no lock held.

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

  std::shared_ptr<GenAIModelInstance> loaded;
  {
    // Windows EP bootstrappers redirect the process-global DLL search path. Serialize that preparation with
    // every OGA model construction, including CPU/default construction that could otherwise observe another
    // load's temporary path. This is deliberately outside mutex_; POSIX receives a no-op guard.
    [[maybe_unused]] auto lifecycle_lock = AcquireModelEpLifecycleLock();

    if (!required_ep.empty() && !ep_detector_.PrepareForModelLoad(required_ep)) {
      FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INTERNAL,
                       "failed to prepare ", required_ep, " for model loading");
    }

    // std::make_shared cannot access the private constructor; using new directly is intentional.
    loaded.reset(new GenAIModelInstance(id_str, path_str, std::move(genai_config), resolved_ep, logger_));
  }

  // Phase 3 — publish under the lock. Re-check shutdown: it may have begun while we were loading outside the
  // lock, and a model must not be published into a shutting-down manager.
  GenAIModelInstance* raw_ptr = nullptr;
  LoadStatus publish_status = LoadStatus::kSuccess;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!admission_closed_) {
      // Keep the local strong reference until after unlock. Even if map insertion throws or unexpectedly finds
      // an existing key, destruction of the newly built engine can therefore happen only outside mutex_.
      const auto [it, inserted] = loaded_models_.emplace(id_str, loaded);
      raw_ptr = it->second.get();
      publish_status = inserted ? LoadStatus::kSuccess : LoadStatus::kModelAlreadyLoaded;
    }
  }

  // The map owns a reference on success; on rejection (or the defensive already-loaded case) this is the
  // point where the newly built engine is torn down, with no manager lock held.
  loaded.reset();

  if (raw_ptr != nullptr) {
    if (publish_status == LoadStatus::kSuccess) {
      logger_.Log(LogLevel::Information, fmt::format("model loaded successfully: {}", id_str));
    }

    return {publish_status, raw_ptr};
  }

  FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot load model during shutdown");
}

// ---------------------------------------------------------------------------
// UnloadModel
// ---------------------------------------------------------------------------

bool ModelLoadManager::UnloadModel(std::string_view model_id) {
  const std::string id_str(model_id);

  enum class Decision {
    kNotLoaded,
    kInUse,
    kDetached,
  };

  Decision decision = Decision::kNotLoaded;
  int live_sessions = 0;

  // Declared before detached so its destructor runs afterwards: the unload reservation stays engaged until
  // physical engine teardown has completed outside mutex_.
  UnloadReservationGuard unload_reservation;
  std::shared_ptr<GenAIModelInstance> detached;

  {
    std::unique_lock<std::mutex> lock(mutex_);

    // Wait for the construction owner and every same-ID caller already admitted behind it. Once this predicate
    // wins while holding mutex_, a later LoadModel either linearizes after this detach or completes before it.
    load_cv_.wait(lock, [&] {
      return !active_load_ids_.contains(id_str) && !unloading_ids_.contains(id_str);
    });

    auto it = loaded_models_.find(id_str);
    if (it != loaded_models_.end()) {
      // Manager mutex -> model session mutex is the one allowed nested order. Lease release takes only the
      // model mutex, so there is no inverse path.
      live_sessions = it->second->SessionRefCount();
      if (live_sessions > 0) {
        decision = Decision::kInUse;
      } else {
        unloading_ids_.insert(id_str);
        unload_reservation.Engage(*this, id_str);
        detached = std::move(it->second);
        loaded_models_.erase(it);
        decision = Decision::kDetached;
      }
    }
  }

  if (decision == Decision::kNotLoaded) {
    logger_.Log(LogLevel::Information, fmt::format("model was not loaded: {}", id_str));
    return false;
  }

  if (decision == Decision::kInUse) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                     "cannot unload model '", id_str, "': ", live_sessions,
                     " session(s) still using it");
  }

  logger_.Log(LogLevel::Information, fmt::format("unloading model: {}", id_str));

  // `detached` dies here: the GenAIModelInstance destructor releases OGA objects in reverse order, with no
  // manager lock held.
  return true;
}

void ModelLoadManager::UnloadAll(std::chrono::milliseconds timeout) {
  // This method is safe as the first shutdown call: close admission and wait until every admitted loader or
  // same-ID waiter has left before taking a model snapshot.
  CloseLoadAdmission();
  DrainModelLifecycleWork();

  // Snapshot *strong* entry references, not raw pointers: each entry now stays alive for as long as this
  // snapshot holds it, so waiting on its condition variable can never race the entry's destruction.
  std::vector<std::pair<std::string, std::shared_ptr<GenAIModelInstance>>> snapshot;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.reserve(loaded_models_.size());
    for (const auto& [id, instance] : loaded_models_) {
      snapshot.emplace_back(id, instance);
    }
  }

  if (snapshot.empty()) {
    return;
  }

  logger_.Log(LogLevel::Information, fmt::format("Shutdown: unloading {} model(s)", snapshot.size()));

  // One overall deadline shared across all models: shutdown stays bounded regardless of how many models are
  // loaded, and a stuck caller on one model cannot extend total drain time linearly with model count.
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  // Entries detached from the map here and destroyed at the end of the function, outside the manager lock.
  std::vector<std::shared_ptr<GenAIModelInstance>> detached;
  detached.reserve(snapshot.size());
  std::vector<std::string> unloaded_ids;
  unloaded_ids.reserve(snapshot.size());

  for (auto& [id, instance] : snapshot) {
    // Blocks on the entry's own CV instead of polling; a lease release wakes it immediately.
    if (!instance->WaitForNoSessions(deadline)) {
      logger_.Log(LogLevel::Warning,
                  fmt::format("Shutdown: model '{}' still has {} session(s) after overall {}ms deadline; "
                              "leaving loaded",
                              id, instance->SessionRefCount(), timeout.count()));
      continue;
    }

    int rechecked_sessions = 0;
    bool did_detach = false;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = loaded_models_.find(id);

      // Re-check identity and lease count atomically against AcquireLoadedModel. Manager mutex -> model session
      // mutex is intentional and consistent with UnloadModel/AcquireLoadedModel.
      if (it != loaded_models_.end() && it->second.get() == instance.get()) {
        rechecked_sessions = it->second->SessionRefCount();
        if (rechecked_sessions == 0) {
          detached.push_back(std::move(it->second));
          loaded_models_.erase(it);
          did_detach = true;
        }
      }
    }

    if (did_detach) {
      unloaded_ids.push_back(id);
    } else if (rechecked_sessions > 0) {
      logger_.Log(LogLevel::Warning,
                  fmt::format("Shutdown: model '{}' acquired {} session(s) during drain; leaving loaded",
                              id, rechecked_sessions));
    }
  }

  for (const auto& id : unloaded_ids) {
    logger_.Log(LogLevel::Information, fmt::format("unloading model: {}", id));
  }

  // `detached` and `snapshot` die here — every GenAIModelInstance destructor runs with no manager lock held.
}

// ---------------------------------------------------------------------------
// GetLoadedModel
// ---------------------------------------------------------------------------

GenAIModelInstance* ModelLoadManager::GetLoadedModel(std::string_view model_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  const std::string id_str(model_id);
  auto it = loaded_models_.find(id_str);
  if (it != loaded_models_.end()) {
    return it->second.get();
  }

  return nullptr;
}

// ---------------------------------------------------------------------------
// AcquireLoadedModel
// ---------------------------------------------------------------------------

std::optional<ModelSessionLease> ModelLoadManager::AcquireLoadedModel(std::string_view model_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Shutdown check, loaded check and lease increment are one critical section. UnloadModel takes the same
  // mutex before it inspects the lease count, so the two can never interleave into "unloaded but leased".
  if (admission_closed_) {
    return std::nullopt;
  }

  auto it = loaded_models_.find(std::string(model_id));
  if (it == loaded_models_.end()) {
    return std::nullopt;
  }

  return ModelSessionLease(it->second);
}

}  // namespace fl
