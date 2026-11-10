// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/model_load_manager.h"
#include "inferencing/predictive/non_generative_runtime.h"
#include "model.h"

#include "exception.h"
#include "inferencing/generative/chat/onnx_chat_engine.h"
#include "inferencing/generative/chat/search_options.h"
#include "inferencing/generative/genai_config.h"
#include "inferencing/generative/genai_model_instance.h"
#include "util/path_safety.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <thread>
#include <utility>

namespace fl {

class ManagedModelRuntime {
 public:
  virtual ~ManagedModelRuntime() = default;
  virtual std::string_view ModelId() const noexcept = 0;
  virtual std::string_view ModelPath() const noexcept = 0;
  virtual GenAIModelInstance* GenAI() const noexcept { return nullptr; }
  virtual const GenAIConfig* Config() const noexcept { return nullptr; }
  virtual size_t SessionRefCount() const noexcept = 0;
  virtual std::shared_ptr<void> Lease() {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "runtime does not support non-generative leases");
  }
};

namespace {

/// The expected config filename inside a model directory.
constexpr const char* kGenAIConfigFileName = "genai_config.json";

std::string GenerativeRuntimeKey(std::string_view model_id, std::string_view model_path) {
  auto path = NormalizeModelPath(model_path);
#ifdef _WIN32
  path = ToLower(std::move(path));
#endif
  return "generative|" + std::to_string(model_id.size()) + ":" + std::string(model_id) + "|" + path;
}

class ManagedGenerativeRuntime final : public ManagedModelRuntime {
 public:
  explicit ManagedGenerativeRuntime(
      std::unique_ptr<GenAIModelInstance> instance)
      : instance_(std::move(instance)) {}

  std::string_view ModelId() const noexcept override {
    return instance_->ModelId();
  }
  std::string_view ModelPath() const noexcept override {
    return instance_->ModelPath();
  }
  GenAIModelInstance* GenAI() const noexcept override {
    return instance_.get();
  }
  const GenAIConfig* Config() const noexcept override {
    return &instance_->GetGenAIConfig();
  }
  size_t SessionRefCount() const noexcept override {
    return instance_->SessionRefCount();
  }

 private:
  std::unique_ptr<GenAIModelInstance> instance_;
};

class ManagedTypedRuntime final
    : public ManagedModelRuntime,
      public std::enable_shared_from_this<ManagedTypedRuntime> {
 public:
  ManagedTypedRuntime(
      std::string model_id, std::string model_path,
      std::shared_ptr<NonGenerativeAdmissionReservation> reservation,
      std::shared_ptr<void> runtime,
      std::shared_ptr<const GenAIConfig> config)
      : model_id_(std::move(model_id)),
        model_path_(std::move(model_path)),
        reservation_(std::move(reservation)),
        runtime_(std::move(runtime)),
        config_(std::move(config)) {}

  ~ManagedTypedRuntime() override {
    runtime_.reset();
    reservation_.reset();
  }

  std::string_view ModelId() const noexcept override { return model_id_; }
  std::string_view ModelPath() const noexcept override { return model_path_; }
  size_t SessionRefCount() const noexcept override {
    const auto owners = weak_from_this().use_count();
    return owners > 0 ? owners - 1 : 0;
  }
  const GenAIConfig* Config() const noexcept override {
    return config_.get();
  }
  std::shared_ptr<void> Lease() override {
    return std::shared_ptr<void>(shared_from_this(), runtime_.get());
  }

 private:
  std::string model_id_;
  std::string model_path_;
  std::shared_ptr<NonGenerativeAdmissionReservation> reservation_;
  std::shared_ptr<void> runtime_;
  std::shared_ptr<const GenAIConfig> config_;
};

std::pair<std::string, std::string> SelectionIdentity(std::string_view model_id, std::string_view model_path) {
  auto path = NormalizeModelPath(model_path);
#ifdef _WIN32
  path = ToLower(std::move(path));
#endif
  return {std::string(model_id), std::move(path)};
}

}  // namespace

LoadedSelectionReservation::LoadedSelectionReservation(ModelLoadManager& manager,
                                                       std::pair<std::string, std::string> identity)
    : manager_(manager), identity_(std::move(identity)) {}

LoadedSelectionReservation::~LoadedSelectionReservation() {
  if (active_) {
    manager_.CompleteSelectionReservation(identity_, false);
  }
}

void LoadedSelectionReservation::Commit() noexcept {
  if (active_) {
    manager_.CompleteSelectionReservation(identity_, true);
    active_ = false;
  }
}

ModelLoadManager::PredictiveRequestLease::PredictiveRequestLease(
    PredictiveRequestLease&& other) noexcept
    : manager_(std::exchange(other.manager_, nullptr)) {}

ModelLoadManager::PredictiveRequestLease::~PredictiveRequestLease() {
  if (manager_) manager_->ReleasePredictiveRequest();
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

ModelLoadManager::ModelLoadManager(
    IEpDetector& ep_detector, ILogger& logger,
    uint64_t non_generative_memory_budget_bytes,
    size_t non_generative_readiness_concurrency)
    : ep_detector_(ep_detector),
      logger_(logger),
      non_generative_admission_(
          std::make_shared<NonGenerativeAdmissionCoordinator>(
              non_generative_memory_budget_bytes,
              non_generative_readiness_concurrency)) {}

ModelLoadManager::~ModelLoadManager() {
  // Destroy all loaded models under the lock.
  std::lock_guard<std::mutex> lock(mutex_);
  loaded_runtimes_.clear();
  non_generative_packages_.clear();
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

void ModelLoadManager::RegisterPredictiveSession(
    void* identity, std::function<void()> cancel,
    std::function<void()> release) {
  std::lock_guard lock(predictive_mutex_);
  if (shutdown_.load())
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "cannot create non-generative session during shutdown");
  predictive_sessions_.insert_or_assign(
      identity,
      PredictiveSessionCallbacks{std::move(cancel), std::move(release)});
}

void ModelLoadManager::UnregisterPredictiveSession(void* identity) noexcept {
  std::lock_guard lock(predictive_mutex_);
  predictive_sessions_.erase(identity);
}

ModelLoadManager::PredictiveRequestLease
ModelLoadManager::AcquirePredictiveRequest() {
  std::lock_guard lock(predictive_mutex_);
  if (shutdown_.load())
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "cannot process non-generative request during shutdown");
  ++active_predictive_requests_;
  return PredictiveRequestLease(*this);
}

void ModelLoadManager::ReleasePredictiveRequest() noexcept {
  std::lock_guard lock(predictive_mutex_);
  --active_predictive_requests_;
  predictive_condition_.notify_all();
}

void ModelLoadManager::WaitForPredictiveRequests() {
  std::unique_lock lock(predictive_mutex_);
  predictive_condition_.wait(
      lock, [&] { return active_predictive_requests_ == 0; });
}

void ModelLoadManager::ReleasePredictiveSessions() {
  std::vector<std::function<void()>> releases;
  {
    std::lock_guard lock(predictive_mutex_);
    for (const auto& entry : predictive_sessions_)
      releases.push_back(entry.second.release);
  }
  for (const auto& release : releases) release();
}

void ModelLoadManager::RunWithUnloadedModelsInDirectory(std::string_view directory,
                                                        const std::function<void()>& operation) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto target = std::filesystem::path(directory);
  for (const auto& [key, runtime] : loaded_runtimes_) {
    if (IsPathWithinDirectory(runtime->ModelPath(), target)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
               "cannot remove a loaded model from cache; unload it first: " +
                   std::string(runtime->ModelId()));
    }
  }
  for (const auto& [key, loading] : loading_runtimes_) {
    if (IsPathWithinDirectory(loading.model_path, target)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
               "cannot remove a loading model from cache: " +
                   loading.model_id);
    }
  }
  for (auto it = non_generative_packages_.begin();
       it != non_generative_packages_.end();) {
    if (IsPathWithinDirectory(it->first.second, target))
      it = non_generative_packages_.erase(it);
    else
      ++it;
  }

  operation();
}

void ModelLoadManager::PrepareProviderForModelLoad(
    std::string_view registration_name, std::string_view model_id) {
  if (registration_name.empty()) return;
  if (!HasEP(std::string(registration_name))) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                     "model '", model_id, "' requires ", registration_name,
                     " which is not registered. Call DownloadAndRegisterEps() first.");
  }

  if (!ep_detector_.PrepareForModelLoad(registration_name)) {
    FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INTERNAL,
                     "failed to prepare ", registration_name,
                     " for model loading");
  }
}

std::shared_ptr<void> ModelLoadManager::AcquireNonGenerativeRuntime(
    Model& model,
    const std::function<void()>& on_admitted) {
  const auto canonical =
      std::filesystem::weakly_canonical(model.GetPath()).string();
  const auto package = InspectNonGenerativePackage(model);
  const auto task = model.Info().task;
  if (!package.task.empty() && package.task != task)
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "package task does not match registered model task");
  const auto provider_name = package.execution_provider;
  const auto identity =
      "non-generative|" + model.Id() + "|" + canonical + "|" +
      provider_name + "|" + task;
  return AcquireManagedRuntime(
      identity, model.Id(), canonical, package.estimated_resident_bytes,
      provider_name.empty() ? "cpu" : provider_name,
      [&]() -> std::shared_ptr<void> {
        const auto registration =
            provider_name.empty()
                ? std::string_view{}
                : EPUtils::EPtoRegistrationName(
                      EPUtils::StringtoEP(provider_name));
        PrepareProviderForModelLoad(registration, model.Id());
        if (task == "text-ranking")
          return std::make_shared<RankingRuntime>(canonical);
        if (task == "typed-decision")
          return std::make_shared<DecisionRuntime>(canonical);
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                 "unsupported non-generative task: ", task);
      },
      on_admitted,
      std::make_shared<GenAIConfig>(package.config));
}

std::shared_ptr<void>
ModelLoadManager::AcquireLoadedNonGenerativeRuntime(Model& model) {
  const auto model_path = NormalizeModelPath(model.GetPath());
  std::lock_guard lock(mutex_);
  const auto found = std::find_if(
      loaded_runtimes_.begin(), loaded_runtimes_.end(),
      [&](const auto& entry) {
        return !entry.second->GenAI() &&
               entry.second->ModelId() == model.Id() &&
               ModelPathsEqual(entry.second->ModelPath(), model_path);
      });
  if (found == loaded_runtimes_.end())
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "model must be loaded before creating a session");
  return found->second->Lease();
}

NonGenerativePackageMetadata
ModelLoadManager::InspectNonGenerativePackage(Model& model) {
  const auto path = NormalizeModelPath(model.GetPath());
  const auto identity = SelectionIdentity(model.Id(), path);
  {
    std::lock_guard lock(mutex_);
    const bool warm =
        std::any_of(
            loaded_runtimes_.begin(), loaded_runtimes_.end(),
            [&](const auto& entry) {
              return entry.second->ModelId() == model.Id() &&
                     ModelPathsEqual(entry.second->ModelPath(), path);
            }) ||
        std::any_of(
            loading_runtimes_.begin(), loading_runtimes_.end(),
            [&](const auto& entry) {
              return entry.second.model_id == model.Id() &&
                     ModelPathsEqual(entry.second.model_path, path);
            });
    if (warm) {
      if (const auto cached = non_generative_packages_.find(identity);
          cached != non_generative_packages_.end())
        return cached->second;
    }
  }

  const auto package = ReadNonGenerativePackage(path);
  if (!package)
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "model is not a non-generative package");
  {
    std::lock_guard lock(mutex_);
    non_generative_packages_.insert_or_assign(identity, *package);
  }
  return *package;
}

std::shared_ptr<void> ModelLoadManager::AcquireManagedRuntime(
    std::string identity, std::string model_id, std::string model_path,
    uint64_t estimated_resident_bytes, std::string readiness_group,
    const std::function<std::shared_ptr<void>()>& factory,
    const std::function<void()>& on_admitted,
    std::shared_ptr<const GenAIConfig> config) {
  const auto artifact_identity = SelectionIdentity(model_id, model_path);
  std::shared_future<std::shared_ptr<ManagedModelRuntime>> future;
  std::shared_ptr<std::promise<std::shared_ptr<ManagedModelRuntime>>> promise;
  std::shared_ptr<ManagedModelRuntime> loaded;
  {
    std::lock_guard lock(mutex_);
    if (shutdown_.load())
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
               "cannot acquire non-generative runtime during shutdown");
    if (const auto loaded_found = loaded_runtimes_.find(identity);
        loaded_found != loaded_runtimes_.end()) {
      loaded = loaded_found->second;
    } else if (const auto loading_found = loading_runtimes_.find(identity);
               loading_found != loading_runtimes_.end()) {
      future = loading_found->second.runtime;
    } else {
      promise = std::make_shared<
          std::promise<std::shared_ptr<ManagedModelRuntime>>>();
      future = promise->get_future().share();
      loading_runtimes_.emplace(
          identity, LoadingRuntimeEntry{model_id, model_path, future});
    }
  }
  if (on_admitted) on_admitted();
  if (loaded) return loaded->Lease();

  if (promise) {
    try {
      auto reservation =
          std::make_shared<NonGenerativeAdmissionReservation>(
              non_generative_admission_, estimated_resident_bytes, model_id);
      NonGenerativeReadinessLease readiness(
          non_generative_admission_, std::move(readiness_group));
      auto runtime = factory();
      auto managed = std::make_shared<ManagedTypedRuntime>(
          std::move(model_id), std::move(model_path), std::move(reservation),
          std::move(runtime), std::move(config));
      {
        std::lock_guard lock(mutex_);
        if (shutdown_.load())
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                   "cannot acquire non-generative runtime during shutdown");
        loaded_runtimes_[identity] = managed;
        loading_runtimes_.erase(identity);
      }
      promise->set_value(managed);
    } catch (...) {
      {
        std::lock_guard lock(mutex_);
        loading_runtimes_.erase(identity);
        non_generative_packages_.erase(artifact_identity);
      }
      promise->set_exception(std::current_exception());
      throw;
    }
  }

  return future.get()->Lease();
}

bool ModelLoadManager::UnloadNonGenerativeRuntime(Model& model) {
  return UnloadModelArtifact(model.Id(), model.GetPath());
}

bool ModelLoadManager::IsModelLoaded(const Model& model) const {
  return IsModelArtifactLoaded(model.Id(), model.GetPath());
}

bool ModelLoadManager::IsModelLoading(const Model& model) const {
  const auto model_path = NormalizeModelPath(model.GetPath());
  std::lock_guard lock(mutex_);
  return std::any_of(
      loading_runtimes_.begin(), loading_runtimes_.end(),
      [&](const auto& entry) {
        return entry.second.model_id == model.Id() &&
               ModelPathsEqual(entry.second.model_path, model_path);
      });
}

bool ModelLoadManager::IsModelArtifactLoaded(
    std::string_view model_id, std::string_view model_path) const {
  const auto normalized_path = NormalizeModelPath(model_path);
  std::lock_guard lock(mutex_);
  return std::any_of(
      loaded_runtimes_.begin(), loaded_runtimes_.end(),
      [&](const auto& entry) {
        return entry.second->ModelId() == model_id &&
               ModelPathsEqual(entry.second->ModelPath(), normalized_path);
      });
}

bool ModelLoadManager::UnloadModelArtifact(
    std::string_view model_id, std::string_view model_path) {
  const auto normalized_path = NormalizeModelPath(model_path);
  if (GetLoadedModel(model_id, normalized_path))
    return UnloadModel(model_id, normalized_path);

  std::vector<std::shared_ptr<ManagedModelRuntime>> removed;
  {
    std::lock_guard lock(mutex_);
    if (std::any_of(
            loading_runtimes_.begin(), loading_runtimes_.end(),
            [&](const auto& entry) {
              return entry.second.model_id == model_id &&
                     ModelPathsEqual(entry.second.model_path, normalized_path);
            }))
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
               "cannot unload model '", model_id, "' while it is loading");
    for (auto it = loaded_runtimes_.begin(); it != loaded_runtimes_.end();) {
      if (it->second->GenAI() || it->second->ModelId() != model_id ||
          !ModelPathsEqual(it->second->ModelPath(), normalized_path)) {
        ++it;
        continue;
      }
      const auto sessions = it->second->SessionRefCount();
      if (sessions != 0)
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                 "cannot unload model '", model_id, "': ", sessions,
                 " session(s) still using it");
      removed.push_back(std::move(it->second));
      it = loaded_runtimes_.erase(it);
    }
    if (!removed.empty())
      non_generative_packages_.erase(
          SelectionIdentity(model_id, normalized_path));
  }
  return !removed.empty();
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
  const auto runtime_key = GenerativeRuntimeKey(id_str, path_str);
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = loaded_runtimes_.find(runtime_key);
  if (it != loaded_runtimes_.end()) {
    if (!ModelPathsEqual(path_str, it->second->ModelPath())) {
      FL_LOG_AND_THROW(logger_, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model '", id_str,
                       "' is already loaded from a different path: ", it->second->ModelPath(),
                       "; requested path: ", path_str);
    }

    return {LoadStatus::kModelAlreadyLoaded, it->second->GenAI()};
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
    required_ep = config_provider.empty() ? std::string(EPUtils::RequiredEpForModelId(id_str))
                                          : EPUtils::RequiredEpForConfigProvider(config_provider);
  }

  // OGA can crash or hang if a model is loaded with an unregistered EP.
  PrepareProviderForModelLoad(required_ep, id_str);

  // std::make_unique cannot access the private constructor; using new directly is intentional.
  auto loaded = std::unique_ptr<GenAIModelInstance>(new GenAIModelInstance(id_str,
                                                                           path_str,
                                                                           std::move(genai_config),
                                                                           resolved_ep,
                                                                           logger_));

  auto* raw_ptr = loaded.get();
  loaded_runtimes_[runtime_key] =
      std::make_shared<ManagedGenerativeRuntime>(std::move(loaded));

  logger_.Log(LogLevel::Information, fmt::format("model loaded successfully: {}", id_str));
  return {LoadStatus::kSuccess, raw_ptr};
}

// ---------------------------------------------------------------------------
// UnloadModel
// ---------------------------------------------------------------------------

bool ModelLoadManager::UnloadModel(std::string_view model_id) {
  return UnloadModel(model_id, static_cast<const std::string*>(nullptr));
}

ModelLoadManager::SessionModelReference ModelLoadManager::AcquireSessionModel(std::string_view model_id,
                                                                              std::string_view model_path) {
  const auto normalized_path = NormalizeModelPath(model_path);
  std::lock_guard lock(mutex_);
  auto* model = FindLoadedModelLocked(model_id, &normalized_path);
  if (model) {
    model->AcquireSession();
  }

  return SessionModelReference(model);
}

bool ModelLoadManager::UnloadModel(std::string_view model_id, std::string_view model_path) {
  const auto normalized_model_path = NormalizeModelPath(model_path);
  return UnloadModel(model_id, &normalized_model_path);
}

bool ModelLoadManager::UnloadModel(std::string_view model_id, const std::string* normalized_model_path) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::string id_str(model_id);
  auto it = loaded_runtimes_.end();
  for (auto candidate = loaded_runtimes_.begin(); candidate != loaded_runtimes_.end(); ++candidate) {
    if (candidate->second->GenAI() && candidate->second->ModelId() == model_id &&
        (!normalized_model_path || ModelPathsEqual(*normalized_model_path, candidate->second->ModelPath()))) {
      if (it != loaded_runtimes_.end()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model ID is loaded from multiple sources; specify its path");
      }
      it = candidate;
    }
  }
  if (it == loaded_runtimes_.end()) {
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
  loaded_runtimes_.erase(it);
  return true;
}

void ModelLoadManager::RejectNewLoads() {
  shutdown_.store(true);
  std::vector<std::function<void()>> cancellations;
  {
    std::lock_guard lock(predictive_mutex_);
    for (const auto& entry : predictive_sessions_)
      cancellations.push_back(entry.second.cancel);
  }
  for (const auto& cancel : cancellations) cancel();
}

void ModelLoadManager::UnloadAll(std::chrono::milliseconds timeout) {
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + timeout;
  {
    std::unique_lock lock(predictive_mutex_);
    if (!predictive_condition_.wait_until(
            lock, deadline,
            [&] { return active_predictive_requests_ == 0; })) {
      logger_.Log(
          LogLevel::Warning,
          fmt::format(
              "Shutdown: {} non-generative request(s) remain active after "
              "overall {}ms deadline",
              active_predictive_requests_, timeout.count()));
    }
  }

  std::vector<std::pair<std::string, std::shared_ptr<ManagedModelRuntime>>>
      snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.reserve(loaded_runtimes_.size());
    for (auto it = loaded_runtimes_.begin();
         it != loaded_runtimes_.end();) {
      if (it->second->GenAI()) {
        snapshot.emplace_back(std::string(it->second->ModelId()), it->second);
        ++it;
      } else {
        it = loaded_runtimes_.erase(it);
      }
    }
    non_generative_packages_.clear();
  }

  if (snapshot.empty()) {
    return;
  }

  logger_.Log(LogLevel::Information,
              fmt::format("Shutdown: unloading {} model(s)", snapshot.size()));

  constexpr auto kPollInterval = std::chrono::milliseconds(50);

  // Overall deadline shared across all models — shutdown must be bounded regardless of
  // how many models are loaded. A stuck caller on one model shouldn't extend total drain
  // time linearly with model count.
  for (auto& [id, runtime] : snapshot) {
    while (runtime->SessionRefCount() > 0 && clock::now() < deadline) {
      std::this_thread::sleep_for(kPollInterval);
    }

    auto remaining = runtime->SessionRefCount();
    if (remaining > 0) {
      logger_.Log(LogLevel::Warning,
                  fmt::format("Shutdown: model '{}' still has {} session(s) after overall {}ms deadline; "
                              "leaving loaded",
                              id, remaining, timeout.count()));
      continue;
    }

    try {
      UnloadModel(id, runtime->ModelPath());
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

bool ModelLoadManager::PreserveLoadedSelection(std::string_view model_id, std::string_view model_path, bool preserve) {
  const auto identity = SelectionIdentity(model_id, model_path);
  std::lock_guard lock(mutex_);
  if (preserve) {
    auto& state = preserved_selections_[identity];
    state.generation = ++selection_generation_;
    return std::exchange(state.preserved, true);
  }

  const auto found = preserved_selections_.find(identity);
  if (found == preserved_selections_.end()) {
    return false;
  }

  const bool previous = std::exchange(found->second.preserved, false);
  if (found->second.reservations == 0) {
    preserved_selections_.erase(found);
  }
  return previous;
}

bool ModelLoadManager::HasPreservedLoadedSelection(std::string_view model_id, std::string_view model_path) const {
  const auto identity = SelectionIdentity(model_id, model_path);
  std::lock_guard lock(mutex_);
  return preserved_selections_.contains(identity);
}

std::unique_ptr<LoadedSelectionReservation> ModelLoadManager::ReserveLoadedSelection(std::string_view model_id,
                                                                                     std::string_view model_path) {
  auto reservation = std::unique_ptr<LoadedSelectionReservation>(
      new LoadedSelectionReservation(*this, SelectionIdentity(model_id, model_path)));
  std::lock_guard lock(mutex_);
  ++preserved_selections_[reservation->identity_].reservations;
  reservation->active_ = true;
  return reservation;
}

void ModelLoadManager::ReconcileLoadedSelections(
    const std::function<bool(const std::string&, const std::string&)>& is_loaded) {
  std::vector<std::pair<std::pair<std::string, std::string>, uint64_t>> selections;
  {
    std::lock_guard lock(mutex_);
    for (const auto& [identity, state] : preserved_selections_) {
      if (state.preserved && state.reservations == 0) {
        selections.emplace_back(identity, state.generation);
      }
    }
  }

  for (const auto& [identity, generation] : selections) {
    if (is_loaded(identity.first, identity.second)) {
      continue;
    }

    std::lock_guard lock(mutex_);
    const auto found = preserved_selections_.find(identity);
    // A load admitted or committed during the remote observation owns a newer selection.
    if (found != preserved_selections_.end() && found->second.reservations == 0 &&
        found->second.generation == generation) {
      preserved_selections_.erase(found);
    }
  }
}

void ModelLoadManager::CompleteSelectionReservation(const std::pair<std::string, std::string>& identity,
                                                    bool commit) noexcept {
  std::lock_guard lock(mutex_);
  const auto found = preserved_selections_.find(identity);
  auto& state = found->second;
  state.preserved = state.preserved || commit;
  if (commit) {
    state.generation = ++selection_generation_;
  }
  --state.reservations;
  if (!state.preserved && state.reservations == 0) {
    preserved_selections_.erase(found);
  }
}

GenAIModelInstance* ModelLoadManager::GetLoadedModel(std::string_view model_id,
                                                     const std::string* normalized_model_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  return FindLoadedModelLocked(model_id, normalized_model_path);
}

GenAIModelInstance* ModelLoadManager::FindLoadedModelLocked(std::string_view model_id,
                                                            const std::string* normalized_model_path) {
  GenAIModelInstance* result = nullptr;
  for (const auto& [key, runtime] : loaded_runtimes_) {
    if (runtime->GenAI() && runtime->ModelId() == model_id &&
        (!normalized_model_path || ModelPathsEqual(*normalized_model_path, runtime->ModelPath()))) {
      if (result) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model ID is loaded from multiple sources; specify its path");
      }
      result = runtime->GenAI();
    }
  }

  return result;
}

std::optional<ModelLoadManager::LoadedModelState> ModelLoadManager::GetLoadedModelState(
    std::string_view model_id, std::string_view model_path) {
  const auto normalized_model_path = NormalizeModelPath(model_path);
  std::lock_guard<std::mutex> lock(mutex_);
  auto* loaded = FindLoadedModelLocked(model_id, &normalized_model_path);
  std::optional<LoadedModelState> state;
  if (loaded) {
    state = LoadedModelState{.config = loaded->GetGenAIConfig(),
                             .model_path = loaded->ModelPath(),
                             .context_length = std::nullopt};
  } else {
    const auto found = std::find_if(
        loaded_runtimes_.begin(), loaded_runtimes_.end(),
        [&](const auto& entry) {
          return !entry.second->GenAI() &&
                 entry.second->ModelId() == model_id &&
                 ModelPathsEqual(entry.second->ModelPath(),
                                 normalized_model_path);
        });
    if (found == loaded_runtimes_.end()) return std::nullopt;
    const auto* config = found->second->Config();
    if (!config) return std::nullopt;
    state = LoadedModelState{
        .config = *config,
        .model_path = std::string(found->second->ModelPath()),
        .context_length = std::nullopt};
  }

  if (loaded &&
      state->config.GetChatBackendKind() == ChatBackendKind::kEngine) {
    // Model construction waits for Engine capability initialization; this cached limit is immutable afterward.
    const auto* engine = loaded->GetChatEngine();
    if (engine && engine->MaxRequestLength() > 0) {
      state->context_length = engine->MaxRequestLength();
    }
  } else if ((state->config.model &&
              state->config.model->context_length > 0) ||
             (state->config.search &&
              state->config.search->max_length > 0)) {
    state->context_length =
        static_cast<uint64_t>(GetModelMaxContextLength(state->config));
  }

  return state;
}

}  // namespace fl
