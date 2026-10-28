// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "api/hosted_backend.h"

#include "catalog.h"
#include "ep_detection/ep_types.h"
#include "exception.h"
#include "hosting/sdk_values.h"
#include "items/item.h"
#include "items/text_item.h"
#include "util/path_safety.h"
#include "util/scope_guard.h"

#include <filesystem>
#include <exception>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace fl::api {
namespace {

using namespace hosting;
using Json = nlohmann::json;

const char* CatalogName(CatalogType catalog) {
  switch (catalog) {
    case CatalogType::kPublic:
      return "public";
    case CatalogType::kLocal:
      return "local";
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unknown catalog type");
}

class HostedState {
 public:
  HostedState(std::shared_ptr<ManagerBackend> metadata, HostedConnector connector)
      : metadata(std::move(metadata)), connector_(std::move(connector)) {
    if (!this->metadata || this->metadata->Kind() != BackendKind::Embedded || !connector_) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "hosted backend requires local metadata and a connector");
    }
  }

  std::shared_ptr<HostedContext> Connect(bool acquire) const {
    std::lock_guard lock(mutex_);
    RequireOpen();
    if (owner_) {
      // Even a failed owner remains terminal. Never rebind children or replay an uncertain operation.
      return owner_;
    }

    auto context = connector_(acquire);
    if (acquire) {
      if (!context) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "host connector did not acquire an owner connection");
      }

      owner_ = context;
    }

    return context;
  }

  void Close() noexcept {
    std::shared_ptr<HostedContext> owner;
    {
      std::lock_guard lock(mutex_);
      closed_.store(true, std::memory_order_release);
      owner = std::move(owner_);
    }

    if (owner) {
      owner->Close();
    }
  }

  bool IsClosed() const noexcept { return closed_.load(std::memory_order_acquire); }

  void RefreshCatalog(CatalogType catalog) const {
    RequireOpen();
    if (catalog == CatalogType::kPublic) {
      const auto context = Connect(false);
      metadata->SetCatalogRuntimeDevices(
          context ? context->Call(Operation::RuntimeDevices, Json::object()).get<DeviceProviders>()
                  : metadata->GetRuntimeDevices());
    }
  }

  void RequireOpen() const {
    if (IsClosed()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "hosted SDK manager is closed");
    }
  }

  const std::shared_ptr<ManagerBackend> metadata;
  std::mutex selection_mutex;
  std::atomic<size_t> downloads{0};
  std::atomic<bool> observed_download{false};
  std::mutex ep_mutex;
  std::map<std::vector<std::string>, std::vector<flEpInfo>> ep_snapshots;

 private:
  const HostedConnector connector_;
  mutable std::mutex mutex_;
  mutable std::shared_ptr<HostedContext> owner_;
  std::atomic<bool> closed_{false};
};

struct IoDescriptors {
  std::vector<std::unique_ptr<Item>> storage;
  std::vector<const Item*> inputs;
  std::vector<const Item*> outputs;

  explicit IoDescriptors(const Json& value) {
    Read(value.at("inputs"), inputs);
    Read(value.at("outputs"), outputs);
  }

  InputOutputInfo View() const { return {inputs.data(), inputs.size(), outputs.data(), outputs.size()}; }

 private:
  void Read(const Json& types, std::vector<const Item*>& pointers) {
    for (const auto& descriptor : types) {
      auto item = Item::Create(static_cast<flItemType>(descriptor.at("type").get<int>()));
      if (!item) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid hosted model IO item type");
      }
      if (item->type == FOUNDRY_LOCAL_ITEM_TEXT) {
        const auto text_type = descriptor.at("text_type").get<int>();
        if (text_type < FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT || text_type > FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid hosted model IO text type");
        }

        static_cast<TextItem&>(*item).text_type = static_cast<flTextItemType>(text_type);
      }

      pointers.push_back(item.get());
      storage.push_back(std::move(item));
    }
  }
};

class HostedModel;

class HostedModels {
 public:
  explicit HostedModels(HostedState& state) : state(state) {}
  ModelBackend* Get(CatalogType catalog, ModelBackend* model);
  std::vector<ModelBackend*> Get(CatalogType catalog, const std::vector<ModelBackend*>& models);
  const HostedModel& Unwrap(CatalogType catalog, const ModelBackend& model) const;

  HostedState& state;

 private:
  std::mutex mutex_;
  std::map<std::pair<CatalogType, ModelBackend*>, std::unique_ptr<HostedModel>> models_;
};

class HostedModel final : public ModelBackend {
 public:
  HostedModel(CatalogType catalog, ModelBackend& source, HostedModels& handles)
      : ModelBackend(BackendKind::Hosted),
        catalog_(catalog),
        source_(source),
        handles_(handles),
        state_(handles.state) {}

  HostedModel(CatalogType catalog, std::unique_ptr<ModelBackend> source, HostedModels& handles,
              std::shared_ptr<HostedState> owner)
      : ModelBackend(BackendKind::Hosted),
        owner_(std::move(owner)),
        owned_(std::move(source)),
        catalog_(catalog),
        source_(*owned_),
        handles_(handles),
        state_(*owner_) {}

  const ModelInfo& Info() const override { return source_.Info(); }
  bool IsCached() const override { return source_.IsCached(); }
  void RefreshCachedState() override {
    state_.RequireOpen();
    source_.RefreshCachedState();
  }
  void ForgetCachedState() override {
    state_.RequireOpen();
    source_.ForgetCachedState();
  }
  const std::string& GetPath() const override { return source_.GetPath(); }
  void Download(std::function<int(float)> progress) override {
    state_.RequireOpen();
    const auto owner = state_.metadata;
    source_.Download(std::move(progress));
  }

  bool IsLoaded() const override {
    const auto context = state_.Connect(false);
    return context && context->Call(Operation::IsLoaded, Payload(false)).get<bool>();
  }

  void Load(std::optional<std::string_view> expected_path) override {
    const auto payload = Payload(true, expected_path);
    state_.Connect(true)->Call(Operation::Load, payload);
  }

  void Unload() override {
    const auto payload = Payload(false);
    if (state_.Connect(false)) {
      state_.Connect(true)->Call(Operation::Unload, payload);
    }
  }

  void RemoveFromCache(std::optional<std::string_view> expected_path) override {
    ModelBackend* selected = nullptr;
    Json payload;
    {
      std::lock_guard lock(state_.selection_mutex);
      const auto& id = source_.Info().model_id;
      for (auto* variant : source_.Variants()) {
        if (variant->Info().model_id == id) {
          selected = variant;
          break;
        }
      }

      if (!selected) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model has no selected variant");
      }

      const auto snapshot = state_.metadata->CaptureModel(*selected);
      RequirePath(snapshot->GetPath(), expected_path);
      payload = {{"model", EncodeModelSelection(catalog_, *snapshot)}};
    }

    if (state_.Connect(false)) {
      state_.Connect(true)->Call(Operation::RemoveFromCache, payload);
      selected->ForgetCachedState();
    } else {
      selected->RemoveFromCache(expected_path);
    }
  }

  InputOutputInfo GetInputOutputInfo() const override {
    const auto context = state_.Connect(false);
    if (!context) {
      return source_.GetInputOutputInfo();
    }

    const auto value = context->Call(Operation::ModelInputOutputInfo, Payload(true));
    const auto key = value.dump();
    std::lock_guard lock(io_mutex_);
    auto& descriptors = io_[key];
    if (!descriptors) {
      descriptors = std::make_unique<IoDescriptors>(value);
    }

    return descriptors->View();
  }

  std::vector<ModelBackend*> Variants() const override {
    if (owned_) {
      return {const_cast<HostedModel*>(this)};
    }

    return handles_.Get(catalog_, source_.Variants());
  }

  void SelectVariant(const ModelBackend& variant) override {
    state_.RequireOpen();
    const auto& selected = handles_.Unwrap(catalog_, variant);
    std::lock_guard lock(state_.selection_mutex);
    source_.SelectVariant(selected.source_);
  }

  std::unique_ptr<SessionBackend> CreateSession() const override {
    const auto payload = Payload(true);
    const auto context = state_.Connect(true);
    const auto identity = DecodeObjectIdentity(context->Call(Operation::CreateSession, payload));
    return CreateHostedSession(context, identity, state_.metadata);
  }

  ModelBackend& Source() const { return source_; }
  CatalogType Catalog() const { return catalog_; }
  const HostedModels& Handles() const { return handles_; }

 private:
  static void RequirePath(std::string_view path, std::optional<std::string_view> expected) {
    if (expected && (path.empty() || expected->empty() ? path != *expected : !ModelPathsEqual(path, *expected))) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model artifact changed; refresh the catalog selection");
    }
  }

  Json Payload(bool capture, std::optional<std::string_view> expected_path = std::nullopt) const {
    std::lock_guard lock(state_.selection_mutex);
    if (capture) {
      const auto selected = state_.metadata->CaptureModel(source_);
      RequirePath(selected->GetPath(), expected_path);
      return {{"model", EncodeModelSelection(catalog_, *selected)}};
    }

    // Retired handles may still observe/unload their own exactly path-qualified artifact.
    return {{"model", EncodeModelSelection(catalog_, source_)}};
  }

  std::shared_ptr<HostedState> owner_;
  std::unique_ptr<ModelBackend> owned_;
  const CatalogType catalog_;
  ModelBackend& source_;
  HostedModels& handles_;
  HostedState& state_;
  mutable std::mutex io_mutex_;
  mutable std::map<std::string, std::unique_ptr<IoDescriptors>> io_;
};

ModelBackend* HostedModels::Get(CatalogType catalog, ModelBackend* model) {
  if (!model) {
    return nullptr;
  }

  std::lock_guard lock(mutex_);
  auto& handle = models_[{catalog, model}];
  if (!handle) {
    handle = std::make_unique<HostedModel>(catalog, *model, *this);
  }

  return handle.get();
}

std::vector<ModelBackend*> HostedModels::Get(CatalogType catalog, const std::vector<ModelBackend*>& models) {
  std::vector<ModelBackend*> result;
  result.reserve(models.size());
  for (auto* model : models) {
    result.push_back(Get(catalog, model));
  }

  return result;
}

const HostedModel& HostedModels::Unwrap(CatalogType catalog, const ModelBackend& model) const {
  if (model.Kind() != BackendKind::Hosted) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model belongs to a different backend");
  }

  const auto* hosted = dynamic_cast<const HostedModel*>(&model);
  if (!hosted || &hosted->Handles() != this || hosted->Catalog() != catalog) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model belongs to a different manager or catalog");
  }

  return *hosted;
}

class HostedCatalog final : public CatalogBackend {
 public:
  HostedCatalog(CatalogType type, HostedModels& handles)
      : CatalogBackend(BackendKind::Hosted),
        type_(type),
        handles_(handles),
        state_(handles.state),
        source_(state_.metadata->GetCatalog(type)) {}

  const std::string& GetName() const override { return source_.GetName(); }
  std::vector<ModelBackend*> ListModels() const override {
    state_.RefreshCatalog(type_);
    return handles_.Get(type_, source_.ListModels());
  }
  ModelBackend* GetModel(const std::string& alias) const override {
    state_.RefreshCatalog(type_);
    return handles_.Get(type_, source_.GetModel(alias));
  }
  ModelBackend* GetModelVariant(const std::string& id) const override {
    state_.RefreshCatalog(type_);
    return handles_.Get(type_, source_.GetModelVariant(id));
  }
  ModelBackend* GetLatestVersion(const ModelBackend& model) const override {
    state_.RefreshCatalog(type_);
    return handles_.Get(type_, source_.GetLatestVersion(handles_.Unwrap(type_, model).Source()));
  }
  std::vector<ModelBackend*> GetCachedModels() const override {
    state_.RefreshCatalog(type_);
    return handles_.Get(type_, source_.GetCachedModels());
  }
  std::vector<ModelBackend*> GetModelVersions(const std::string& alias, const std::string& name, int maximum) override {
    state_.RefreshCatalog(type_);
    return handles_.Get(type_, source_.GetModelVersions(alias, name, maximum));
  }

  std::vector<ModelBackend*> GetLoadedModels() const override {
    state_.RefreshCatalog(type_);
    const auto context = state_.Connect(false);
    if (!context) {
      return {};
    }

    std::vector<ModelBackend*> models;
    for (const auto& value : context->Call(Operation::LoadedModels, {{"catalog", CatalogName(type_)}})) {
      const auto selection = DecodeModelSelection(value);
      auto* source = source_.GetModelVariant(selection.info.model_id);
      if (!source || selection.catalog != type_) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "loaded model is unavailable in this client's catalog");
      }

      source->RefreshCachedState();
      const auto captured = state_.metadata->CaptureModel(*source);
      if (captured->Info() != selection.info || !ModelPathsEqual(captured->GetPath(), selection.path)) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "loaded model artifact differs from this client's selection");
      }

      models.push_back(handles_.Get(type_, source));
    }

    return models;
  }

  ModelBackend* RegisterModel(const std::string& path, const std::string& id, const ModelInfo& metadata) override {
    if (!state_.Connect(false)) {
      return handles_.Get(type_, source_.RegisterModel(path, id, metadata));
    }

    const auto context = state_.Connect(true);
    const auto absolute = path.empty() ? path : std::filesystem::absolute(path).string();
    const auto result = context->Call(
        Operation::RegisterModel,
        {{"catalog", CatalogName(type_)}, {"path", absolute}, {"id", id}, {"metadata", EncodeModelInfo(metadata)}});
    const auto selection = DecodeModelSelection(result);
    auto* registered = source_.GetModelVariant(selection.info.model_id);
    if (!registered || selection.catalog != type_) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "host registration is unavailable in this client's catalog");
    }

    return handles_.Get(type_, registered);
  }

  void UnregisterModel(const std::string& alias_or_id) override {
    if (state_.Connect(false)) {
      state_.Connect(true)->Call(Operation::UnregisterModel,
                                 {{"catalog", CatalogName(type_)}, {"alias_or_id", alias_or_id}});
      // Authoritative local catalogs refresh/retire handles from their shared persistent registry.
      static_cast<void>(source_.ListModels());
    } else {
      source_.UnregisterModel(alias_or_id);
    }
  }

 private:
  const CatalogType type_;
  HostedModels& handles_;
  HostedState& state_;
  CatalogBackend& source_;
};

class HostedManager final : public ManagerBackend {
 public:
  HostedManager(std::shared_ptr<ManagerBackend> metadata, HostedConnector connector)
      : ManagerBackend(BackendKind::Hosted),
        state_(std::make_shared<HostedState>(std::move(metadata), std::move(connector))),
        models_(*state_),
        public_catalog_(CatalogType::kPublic, models_),
        local_catalog_(CatalogType::kLocal, models_) {}

  ~HostedManager() override { state_->Close(); }

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
    state_->RequireOpen();
    const auto* hosted = dynamic_cast<const HostedModel*>(&model);
    if (!hosted) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model belongs to a different backend");
    }

    const auto& selected = models_.Unwrap(hosted->Catalog(), model);
    std::lock_guard lock(state_->selection_mutex);
    return std::make_unique<HostedModel>(selected.Catalog(), state_->metadata->CaptureModel(selected.Source()), models_,
                                         state_);
  }

  bool IsModelArtifactLoaded(const std::string& id, const std::string& path) const override {
    const auto context = state_->Connect(false);
    ModelInfo info;
    info.model_id = id;
    return context &&
           context->Call(Operation::IsLoaded, {{"model", EncodeModelSelection({CatalogType::kLocal, info, path})}})
               .get<bool>();
  }

  void UnloadModelArtifact(const std::string& id, const std::string& path) override {
    if (state_->Connect(false)) {
      ModelInfo info;
      info.model_id = id;
      state_->Connect(true)->Call(Operation::Unload,
                                  {{"model", EncodeModelSelection({CatalogType::kLocal, info, path})}});
    }
  }

  void StartWebService() override {
    RetainUrls(state_->Connect(true)->Call(Operation::WebServiceStart, Json::object()).get<std::vector<std::string>>());
  }

  const std::vector<std::string>& GetWebServiceUrls() const override {
    const auto context = state_->Connect(false);
    return RetainUrls(context ? context->Call(Operation::WebServiceUrls, Json::object()).get<std::vector<std::string>>()
                              : std::vector<std::string>{});
  }

  void StopWebService() override {
    if (state_->Connect(false)) {
      state_->Connect(true)->Call(Operation::WebServiceStop, Json::object());
    }

    RetainUrls({});
  }

  std::span<const flEpInfo> GetDiscoverableEps() const override {
    const auto context = state_->Connect(false);
    std::vector<EpInfo> values;
    if (context) {
      const auto snapshot = context->Call(Operation::DiscoverableEps, Json::object());
      state_->observed_download.store(snapshot.at("downloading").get<bool>(), std::memory_order_release);
      for (const auto& ep : snapshot.at("eps")) {
        values.push_back({ep.at("name").get<std::string>(), ep.at("registered").get<bool>()});
      }
    } else {
      state_->observed_download.store(false, std::memory_order_release);
      for (const auto& ep : state_->metadata->GetDiscoverableEps()) {
        values.push_back({ep.name, false});
      }
    }

    std::vector<std::string> names;
    for (const auto& value : values) {
      names.push_back(value.name);
    }

    std::lock_guard lock(state_->ep_mutex);
    auto [snapshot, inserted] = state_->ep_snapshots.try_emplace(std::move(names));
    auto& eps = snapshot->second;
    if (inserted) {
      eps.reserve(values.size());
      for (size_t i = 0; i < values.size(); ++i) {
        eps.push_back({FOUNDRY_LOCAL_DATA_VERSION, snapshot->first[i].c_str(), values[i].is_registered});
      }
    } else {
      for (size_t i = 0; i < values.size(); ++i) {
        eps[i].is_registered = values[i].is_registered;
      }
    }

    return eps;
  }

  DeviceProviders GetRuntimeDevices() const override {
    const auto context = state_->Connect(false);
    return context ? context->Call(Operation::RuntimeDevices, Json::object()).get<DeviceProviders>()
                   : state_->metadata->GetRuntimeDevices();
  }

  void SetCatalogRuntimeDevices(DeviceProviders devices) override {
    state_->RequireOpen();
    state_->metadata->SetCatalogRuntimeDevices(std::move(devices));
  }

  EpDownloadResult DownloadAndRegisterEps(const std::vector<std::string>* names,
                                          const EpProgressCallback& progress) override {
    const auto state = state_;
    const auto context = state->Connect(true);
    state->downloads.fetch_add(1, std::memory_order_acq_rel);
    ScopeGuard completed([state]() noexcept { state->downloads.fetch_sub(1, std::memory_order_acq_rel); });
    const auto id = context->Connection().AllocateInvocation();
    std::exception_ptr callback_error;
    const auto result = context->Connection().Call(
        id, Operation::DownloadAndRegisterEps, {{"names", names ? Json(*names) : Json(nullptr)}},
        [&](auto invocation, const Json& event) {
          bool keep_going = false;
          try {
            keep_going = !progress || progress(event.at("name").get<std::string>(), event.at("percent").get<float>());
          } catch (...) {
            callback_error = std::current_exception();
          }

          context->Connection().PushInput(invocation, {{"continue", keep_going}});
        });
    if (result.status == FOUNDRY_LOCAL_OK) {
      state->observed_download.store(result.value.at("downloading").get<bool>(), std::memory_order_release);
    }

    if (callback_error) {
      std::rethrow_exception(callback_error);
    }
    if (result.status != FOUNDRY_LOCAL_OK) {
      FL_THROW(static_cast<flErrorCode>(result.status), result.error);
    }

    const auto& value = result.value;
    EpDownloadResult downloaded{
        value.at("success").get<bool>(), value.at("cancelled").get<bool>(), value.at("status").get<std::string>(),
        value.at("registered").get<std::vector<std::string>>(), value.at("failed").get<std::vector<std::string>>()};
    {
      std::lock_guard lock(state->ep_mutex);
      for (auto& snapshot : state->ep_snapshots) {
        for (auto& ep : snapshot.second) {
          if (std::find(downloaded.registered_eps.begin(), downloaded.registered_eps.end(), ep.name) !=
              downloaded.registered_eps.end()) {
            ep.is_registered = true;
          }
        }
      }
    }

    return downloaded;
  }

  bool IsEpDownloadInProgress() const override {
    return state_->downloads.load(std::memory_order_acquire) != 0 ||
           state_->observed_download.load(std::memory_order_acquire);
  }
  void Shutdown() override { state_->Close(); }
  bool IsShutdownRequested() const override { return state_->IsClosed(); }

 private:
  const std::vector<std::string>& RetainUrls(std::vector<std::string> urls) const {
    std::lock_guard lock(urls_mutex_);
    return *url_snapshots_.insert(std::move(urls)).first;
  }

  const std::shared_ptr<HostedState> state_;
  HostedModels models_;
  HostedCatalog public_catalog_;
  HostedCatalog local_catalog_;
  mutable std::mutex urls_mutex_;
  mutable std::set<std::vector<std::string>> url_snapshots_;
};

}  // namespace

std::unique_ptr<ManagerBackend> CreateHostedBackend(std::shared_ptr<ManagerBackend> metadata,
                                                    HostedConnector connector) {
  return std::make_unique<HostedManager>(std::move(metadata), std::move(connector));
}

}  // namespace fl::api
