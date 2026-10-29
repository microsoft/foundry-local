// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "api/backend.h"

#include "catalog.h"
#include "catalog/azure_model_catalog.h"
#include "exception.h"
#include "inferencing/session/session.h"
#include "inferencing/model_load_manager.h"
#include "manager.h"

#include <mutex>
#include <unordered_map>
#include <utility>

namespace fl::api {
namespace {

class EmbeddedPreflight final : public PreflightBackend {
 public:
  EmbeddedPreflight(std::unique_ptr<Session::RequestPreflightOperation> operation, std::shared_ptr<Model> model)
      : PreflightBackend(BackendKind::Embedded), model_(std::move(model)), operation_(std::move(operation)) {}

  RequestBudget Execute() override { return operation_->Execute(); }

 private:
  std::shared_ptr<Model> model_;
  std::unique_ptr<Session::RequestPreflightOperation> operation_;
};

class EmbeddedSession final : public SessionBackend {
 public:
  explicit EmbeddedSession(std::unique_ptr<Session> session, std::shared_ptr<Model> model = {})
      : SessionBackend(BackendKind::Embedded),
        model_(std::move(model)),
        owned_(std::move(session)),
        session_(*owned_) {}

  explicit EmbeddedSession(Session& session) : SessionBackend(BackendKind::Embedded), session_(session) {}

  void SetStreamingCallback(flStreamingCallback callback, void* user_data) override {
    session_.SetStreamingCallback(callback, user_data);
  }

  void SetSessionOptions(const KeyValuePairs& options) override { session_.SetSessionOptions(options); }
  void ProcessRequest(const Request& request, Response& response, std::optional<StreamingCallback> callback) override {
    session_.ProcessRequest(request, response, std::move(callback));
  }
  void AddToolDefinition(ToolDefinition definition) override { session_.AddToolDefinition(std::move(definition)); }
  bool RemoveToolDefinition(const std::string& name) override { return session_.RemoveToolDefinition(name); }
  size_t TurnCount() const override { return session_.TurnCount(); }
  void UndoTurns(size_t count) override { session_.UndoTurns(count); }

  std::unique_ptr<PreflightBackend> CreateRequestPreflight(const Request& request) const override {
    return std::make_unique<EmbeddedPreflight>(session_.CreateRequestPreflight(request), model_);
  }

 private:
  std::shared_ptr<Model> model_;
  std::unique_ptr<Session> owned_;
  Session& session_;
};

class EmbeddedModel;

class ModelHandles {
 public:
  ModelBackend* Get(Model* model);
  std::vector<ModelBackend*> Get(const std::vector<Model*>& models);

 private:
  std::mutex mutex_;
  std::unordered_map<Model*, std::unique_ptr<EmbeddedModel>> models_;
};

class EmbeddedModel final : public ModelBackend {
 public:
  EmbeddedModel(Model& model, ModelHandles& handles)
      : ModelBackend(BackendKind::Embedded), model_(model), handles_(handles) {}

  EmbeddedModel(std::shared_ptr<Model> model, ModelHandles& handles)
      : ModelBackend(BackendKind::Embedded), owned_(std::move(model)), model_(*owned_), handles_(handles) {}

  const ModelInfo& Info() const override { return model_.Info(); }
  InputOutputInfo GetInputOutputInfo() const override {
    const auto info = model_.GetInputOutputInfo();
    return {info.inputs, info.num_inputs, info.outputs, info.num_outputs};
  }
  bool IsCached() const override { return model_.IsCached(); }
  void RefreshCachedState() override { model_.RefreshCachedState(); }
  void ForgetCachedState() override { model_.ForgetCachedState(); }
  const std::string& GetPath() const override { return model_.GetPath(); }
  void Download(std::function<int(float)> progress) override { model_.Download(std::move(progress)); }
  bool IsLoaded() const override { return model_.IsLoaded(); }
  void Load(std::optional<std::string_view> expected_path) override {
    model_.Load(ExecutionProvider::kDefault, expected_path);
  }
  void Unload() override { model_.Unload(); }
  void RemoveFromCache(std::optional<std::string_view> expected_path) override { model_.RemoveFromCache(expected_path); }
  std::vector<ModelBackend*> Variants() const override {
    if (owned_) {
      return {const_cast<EmbeddedModel*>(this)};
    }

    return handles_.Get(model_.Variants());
  }

  void SelectVariant(const ModelBackend& variant) override { model_.SelectVariant(Unwrap(variant, handles_)); }

  std::unique_ptr<SessionBackend> CreateSession() const override {
    return std::make_unique<EmbeddedSession>(Session::Create(model_), owned_);
  }

  static const Model& Unwrap(const ModelBackend& model, const ModelHandles& handles) {
    if (model.Kind() != BackendKind::Embedded) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model belongs to a different backend");
    }

    const auto& embedded = static_cast<const EmbeddedModel&>(model);
    if (&embedded.handles_ != &handles) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model belongs to a different manager");
    }

    return embedded.model_;
  }

 private:
  std::shared_ptr<Model> owned_;
  Model& model_;
  ModelHandles& handles_;
};

ModelBackend* ModelHandles::Get(Model* model) {
  if (!model) {
    return nullptr;
  }

  std::lock_guard lock(mutex_);
  const auto found = models_.find(model);
  if (found != models_.end()) {
    return found->second.get();
  }

  auto handle = std::make_unique<EmbeddedModel>(*model, *this);
  auto* result = handle.get();
  models_.emplace(model, std::move(handle));
  return result;
}

std::vector<ModelBackend*> ModelHandles::Get(const std::vector<Model*>& models) {
  std::vector<ModelBackend*> result;
  result.reserve(models.size());
  for (auto* model : models) {
    result.push_back(Get(model));
  }

  return result;
}

class EmbeddedCatalog final : public CatalogBackend {
 public:
  EmbeddedCatalog(ICatalog& catalog, ModelHandles& handles)
      : CatalogBackend(BackendKind::Embedded), catalog_(catalog), handles_(handles) {}

  const std::string& GetName() const override { return catalog_.GetName(); }
  std::vector<ModelBackend*> ListModels() const override { return handles_.Get(catalog_.ListModels()); }
  ModelBackend* GetModel(const std::string& alias) const override { return handles_.Get(catalog_.GetModel(alias)); }

  ModelBackend* GetModelVariant(const std::string& model_id) const override {
    return handles_.Get(catalog_.GetModelVariant(model_id));
  }

  ModelBackend* GetLatestVersion(const ModelBackend& model) const override {
    return handles_.Get(catalog_.GetLatestVersion(&EmbeddedModel::Unwrap(model, handles_)));
  }

  std::vector<ModelBackend*> GetCachedModels() const override { return handles_.Get(catalog_.GetCachedModels()); }
  std::vector<ModelBackend*> GetLoadedModels() const override { return handles_.Get(catalog_.GetLoadedModels()); }

  std::vector<ModelBackend*> GetModelVersions(const std::string& alias, const std::string& name,
                                              int max_versions) override {
    return handles_.Get(catalog_.GetModelVersions(alias, name, max_versions));
  }

  ModelBackend* RegisterModel(const std::string& path, const std::string& model_id,
                              const ModelInfo& metadata) override {
    return handles_.Get(catalog_.RegisterModel(path, model_id, metadata));
  }

  void UnregisterModel(const std::string& alias_or_model_id) override { catalog_.UnregisterModel(alias_or_model_id); }

 private:
  ICatalog& catalog_;
  ModelHandles& handles_;
};

struct EmbeddedManagerOwner {
  explicit EmbeddedManagerOwner(const Configuration& configuration) : manager(Manager::Create(configuration)) {}
  ~EmbeddedManagerOwner() { Manager::Destroy(); }

  EmbeddedManagerOwner(const EmbeddedManagerOwner&) = delete;
  EmbeddedManagerOwner& operator=(const EmbeddedManagerOwner&) = delete;

  Manager& manager;
};

class EmbeddedManager final : public ManagerBackend {
 public:
  explicit EmbeddedManager(const Configuration& configuration)
      : ManagerBackend(BackendKind::Embedded),
        owner_(configuration),
        public_catalog_(owner_.manager.GetCatalog(CatalogType::kPublic), models_),
        local_catalog_(owner_.manager.GetCatalog(CatalogType::kLocal), models_) {}

  CatalogBackend& GetCatalog(CatalogType type) override {
    switch (type) {
      case CatalogType::kPublic:
        return public_catalog_;
      case CatalogType::kLocal:
        return local_catalog_;
    }

    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unknown catalog type");
  }

  std::unique_ptr<ModelBackend> CaptureModel(const ModelBackend& model) override {
    auto captured = std::make_shared<Model>(EmbeddedModel::Unwrap(model, models_).CaptureExecutionModel());
    return std::make_unique<EmbeddedModel>(std::move(captured), models_);
  }

  bool IsModelArtifactLoaded(const std::string& model_id, const std::string& path) const override {
    return owner_.manager.GetModelLoadManager().GetLoadedModel(model_id, path) != nullptr;
  }

  void UnloadModelArtifact(const std::string& model_id, const std::string& path) override {
    owner_.manager.GetModelLoadManager().UnloadModel(model_id, path);
  }

  void StartWebService() override { owner_.manager.StartWebService(); }
  const std::vector<std::string>& GetWebServiceUrls() const override { return owner_.manager.GetWebServiceUrls(); }
  void StopWebService() override { owner_.manager.StopWebService(); }

  std::span<const flEpInfo> GetDiscoverableEps() const override {
    return owner_.manager.GetEpDetector().GetDiscoverableEpsCApi();
  }

  DeviceProviders GetRuntimeDevices() const override {
    return owner_.manager.GetEpDetector().GetAvailableDevicesToEPs();
  }

  void SetCatalogRuntimeDevices(DeviceProviders devices) override {
    static_cast<AzureModelCatalog&>(owner_.manager.GetCatalog(CatalogType::kPublic))
        .SetRuntimeDevices(std::move(devices));
  }

  EpDownloadResult DownloadAndRegisterEps(const std::vector<std::string>* names,
                                          const EpProgressCallback& progress) override {
    return owner_.manager.DownloadAndRegisterEps(names, progress);
  }

  bool IsEpDownloadInProgress() const override { return owner_.manager.GetEpDetector().IsDownloadInProgress(); }
  void Shutdown() override { owner_.manager.Shutdown(); }
  bool IsShutdownRequested() const override { return owner_.manager.IsShutdownRequested(); }

 private:
  EmbeddedManagerOwner owner_;
  ModelHandles models_;
  EmbeddedCatalog public_catalog_;
  EmbeddedCatalog local_catalog_;
};

}  // namespace

std::unique_ptr<ManagerBackend> CreateEmbeddedBackend(const Configuration& configuration) {
  return std::make_unique<EmbeddedManager>(configuration);
}

std::unique_ptr<SessionBackend> CreateEmbeddedSession(std::unique_ptr<Session> session) {
  if (!session) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "embedded session must not be null");
  }

  return std::make_unique<EmbeddedSession>(std::move(session));
}

std::unique_ptr<SessionBackend> BorrowEmbeddedSession(Session& session) {
  return std::make_unique<EmbeddedSession>(session);
}

}  // namespace fl::api
