// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/azure_registry_catalog.h"

#include "catalog/azure_catalog_models.h"
#include "catalog/catalog_cache.h"
#include "inferencing/execution_provider.h"
#include "telemetry/telemetry_environment.h"
#include "util/sha256.h"
#include "util/file_lock.h"
#include "utils.h"

#include <azure/identity/azure_cli_credential.hpp>
#include <azure/identity/workload_identity_credential.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <unordered_set>

namespace fl {
namespace {

std::string CacheDirectory(const std::string& registry, const Configuration& config) {
  return (std::filesystem::path(*config.model_cache_dir) /
          (registry + "-" + Sha256String("azureml://registries/" + registry))).string();
}

std::shared_ptr<Azure::Core::Credentials::TokenCredential> Credential(const std::string& mode) {
  if (mode == "workload_identity") {
    return std::make_shared<Azure::Identity::WorkloadIdentityCredential>();
  }
  if ((mode == "azure_cli" || mode.empty()) && !TelemetryEnvironment::IsCiEnvironment()) {
    return std::make_shared<Azure::Identity::AzureCliCredential>();
  }
  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
           "Set catalog credential to azure_cli or workload_identity; CI must explicitly select workload_identity.");
}

std::optional<VariantMetadata> LegacyVariantMetadata(const std::string& name) {
  const auto device_separator = name.rfind('-');
  if (device_separator == std::string::npos || device_separator == 0) return std::nullopt;
  const auto backend_separator = name.rfind('-', device_separator - 1);
  if (backend_separator == std::string::npos) return std::nullopt;
  const auto device = ToLower(name.substr(device_separator + 1));
  const auto backend = ToLower(name.substr(backend_separator + 1, device_separator - backend_separator - 1));
  VariantMetadata variant;
  if (device == "cpu") variant.device = "CPU";
  else if (device == "gpu") variant.device = "GPU";
  else if (device == "npu") variant.device = "NPU";
  else return std::nullopt;

  auto provider = std::string(EPUtils::RequiredEpForModelId(backend + "-" + device));
  if (provider.empty()) {
    if (backend == "generic") {
      if (device == "cpu") provider = "CPUExecutionProvider";
      else if (device == "gpu") provider = "DmlExecutionProvider";
    } else {
      provider = EPUtils::NormalizeRegistrationName(backend);
    }
  }
  if (provider != "DmlExecutionProvider" && EPUtils::StringtoEP(provider) == ExecutionProvider::kUnknown) {
    return std::nullopt;
  }
  variant.execution_provider = std::move(provider);
  variant.model_type = "ONNX";
  return variant;
}

}  // namespace

KeyValuePairs AzureRegistryCatalog::NormalizeOptions(const KeyValuePairs& options) {
  for (const auto& [key, value] : options) {
    if (key != "name" && key != "credential") {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported Azure Registry catalog option: ", key);
    }
    if (value.find('\0') != std::string::npos) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "catalog options must not contain null characters");
    }
  }
  const auto* name = options.Find("name");
  if (!name || !*name) FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Azure Registry catalog requires option 'name'");
  const auto* credential = options.Find("credential");
  const std::string mode = credential ? credential : "azure_cli";
  if (mode != "azure_cli" && mode != "workload_identity") {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "catalog credential must be azure_cli or workload_identity");
  }
  KeyValuePairs result;
  result.Add("name", AzureMlRegistryClient::ValidateRegistryName(name));
  result.Add("credential", mode);
  return result;
}

AzureRegistryCatalog::AzureRegistryCatalog(std::string registry, const Configuration& config,
                               const IEpDetector& ep_detector, ModelLoadManager& load_manager,
                               ILogger& logger, ITelemetry& telemetry,
                               std::unique_ptr<AzureMlRegistryClient> client,
                               std::string credential, bool cache_only)
    : RemoteModelCatalog("azure_registry", CatalogType::kAzureRegistry,
          [this](ModelInfo info, std::string path) {
            info.string_properties["azure_registry.credential"] = credential_mode_.empty() ? "azure_cli" : credential_mode_;
            return Model::FromModelInfo(std::move(info), std::move(path), *downloader_, load_manager_);
          }, ep_detector, logger),
      registry_(std::move(registry)),
      cache_dir_(CacheDirectory(registry_, config)),
      cache_only_(cache_only),
      credential_mode_(std::move(credential)),
      logger_(logger),
      load_manager_(load_manager),
      client_(std::move(client)) {
  int download_concurrency = 64;
  const auto it = config.additional_options.find("NumModelDownloadThreads");
  if (it != config.additional_options.end()) {
    try {
      download_concurrency = std::stoi(it->second);
      if (download_concurrency < 1) {
        download_concurrency = 1;
      }
    } catch (const std::exception&) {
      // Ignore invalid values, use default
    }
  }
  downloader_ = std::make_unique<DownloadManager>(cache_dir_, "", download_concurrency, logger, telemetry, true,
      nullptr, nullptr, [this](const ModelInfo& info) {
        return Client().ResolveArtifacts(info.name, info.version);
      });
}

AzureMlRegistryClient& AzureRegistryCatalog::Client() const {
  if (cache_only_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot access the registry in cache-only mode");
  }
  std::lock_guard lock(client_mutex_);
  if (!client_) client_ = std::make_unique<AzureMlRegistryClient>(registry_, Credential(credential_mode_));
  return *client_;
}

std::optional<ModelInfo> AzureRegistryCatalog::Adapt(const nlohmann::json& model) const {
  const auto& properties = model.at("properties");
  if (!properties.contains("tags") || properties["tags"].is_null()) return std::nullopt;
  const auto& tags = properties["tags"];
  if (!tags.is_object()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Azure ML model tags must be an object");
  }
  bool tagged = tags.contains("foundryLocal");
  for (const auto* key : {"device", "execution_provider", "os", "architecture"}) {
    tagged = tagged || tags.contains(key);
  }
  if (!tagged) return std::nullopt;
  const auto tag = [&](const char* name) {
    const auto it = tags.find(name);
    if (it == tags.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "test model ",
               model.at("foundryVariantName").get<std::string>(), " missing required tag: ", name);
    }
    return it->get<std::string>();
  };
  auto catalog_metadata = model;
  catalog_metadata["annotations"]["tags"] = tags;
  auto normalized = catalog_metadata.get<CatalogLocalModel>();
  normalized.name = model.at("foundryVariantName").get<std::string>();
  const bool legacy = tags.contains("foundryLocal") && !tags.contains("device") &&
      !tags.contains("execution_provider") && !tags.contains("os") && !tags.contains("architecture");
  VariantMetadata variant;
  if (legacy) {
    auto metadata = LegacyVariantMetadata(*normalized.name);
    if (!metadata) {
      logger_.Log(LogLevel::Information, "Skipping legacy model without a supported backend suffix: " +
          *normalized.name);
      return std::nullopt;
    }
    variant = std::move(*metadata);
    for (const auto* key : {"alias", "task"}) {
      const auto it = tags.find(key);
      if (it == tags.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {
        logger_.Log(LogLevel::Warning, "Skipping legacy model " + *normalized.name +
            ": missing or invalid " + key + " tag.");
        return std::nullopt;
      }
    }
    logger_.Log(LogLevel::Information, "Using legacy backend metadata for " + *normalized.name +
        "; platform compatibility will be validated when the package is loaded.");
  } else {
    const auto os = ToLower(tag("os"));
    const auto architecture = ToLower(tag("architecture"));
    if (os != "windows" && os != "linux" && os != "macos" && os != "android") {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid os tag");
    }
    if (architecture != "x64" && architecture != "arm64") {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid architecture tag");
    }
    variant.device = tag("device");
    variant.execution_provider = tag("execution_provider");
    variant.model_type = "ONNX";
  }
  normalized.asset_id = model.at("id").get<std::string>();
  const auto prefix = "azureml://registries/" + registry_ + "/models/" + *normalized.name + "/versions/";
  if (!normalized.asset_id->starts_with(prefix)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "test model asset ID does not match its registry and variant");
  }
  normalized.version = normalized.asset_id->substr(prefix.size());
  normalized.alias = tag("alias");
  normalized.inference_tasks = {tag("task")};
  if (normalized.variant_information && normalized.variant_information->variant_metadata) {
    auto metadata = std::move(*normalized.variant_information->variant_metadata);
    metadata.device = std::move(variant.device);
    metadata.execution_provider = std::move(variant.execution_provider);
    metadata.model_type = std::move(variant.model_type);
    variant = std::move(metadata);
  }
  normalized.variant_information = VariantInformation{{}, variant};
  if (tags.contains("minFLVersion")) normalized.min_fl_version = tag("minFLVersion");
  auto info = CatalogModelToModelInfo(normalized);
  if (!info || info->version <= 0 || info->device_type == DeviceType::kNotSet ||
      (EPUtils::StringtoEP(info->execution_provider) == ExecutionProvider::kUnknown &&
       EPUtils::NormalizeRegistrationName(info->execution_provider) != "DmlExecutionProvider")) {
    if (legacy) {
      logger_.Log(LogLevel::Warning, "Skipping legacy model " + *normalized.name +
          ": unsupported version " + *normalized.version + ".");
      return std::nullopt;
    }
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "test model has invalid version, device or execution provider");
  }
  info->string_properties[kRegistryProperty] = registry_;
  info->string_properties["azureml.registryResourceId"] = Client().ResourceId();
  if (!legacy) {
    info->string_properties["os"] = ToLower(tag("os"));
    info->string_properties["architecture"] = ToLower(tag("architecture"));
  }
  return info;
}

std::vector<Model*> AzureRegistryCatalog::ListModels() const {
  auto models = RemoteModelCatalog::ListModels();
  for (auto* model : models) {
    for (auto* variant : model->Variants()) RefreshRetainedVariantCache(*variant);
  }
  return models;
}

std::vector<Model> AzureRegistryCatalog::CachedVariants(const std::string& alias) const {
  std::lock_guard snapshot_lock(snapshot_mutex_);
  CatalogCache cache(cache_dir_, logger_);
  cache.Load();
  auto infos = cache.GetCachedModels().value_or(std::vector<ModelInfo>{});
  std::erase_if(infos, [&](const ModelInfo& info) {
    return info.alias != alias || GetModelPath(info, Query::Lookup).empty();
  });
  return CreateModels(infos, Query::Lookup);
}

void AzureRegistryCatalog::ReconcileCachedAlias(Model& model) const {
  std::unordered_set<std::string> known_ids;
  for (auto* variant : model.Variants()) {
    known_ids.insert(variant->Id());
    RefreshRetainedVariantCache(*variant);
  }
  for (auto& variant : CachedVariants(model.Alias())) {
    if (!known_ids.contains(variant.Id())) model.AddVariant(std::move(variant));
  }
  model.SelectDefaultVariant(MakeVariantEligibility(), /*preserve_explicit_selection=*/true);
}

Model* AzureRegistryCatalog::GetModel(const std::string& alias) const {
  std::lock_guard lock(alias_models_mutex_);
  auto it = alias_models_.find(alias);
  if (it != alias_models_.end()) {
    ReconcileCachedAlias(*it->second);
    if (it->second->HasEligibleDefaultVariant()) {
      return it->second.get();
    }
    return nullptr;
  }

  const auto make_alias = [&](std::vector<Model> variants) -> Model* {
    if (variants.empty()) return nullptr;
    std::sort(variants.begin(), variants.end(), Model::CompareBestFirst);
    auto container = std::make_unique<Model>(Model::MakeContainer(std::move(variants.front())));
    for (size_t i = 1; i < variants.size(); ++i) container->AddVariant(std::move(variants[i]));
    container->SetDefaultVariantRefresh([this, c = container.get()] {
      EnsurePopulated();
      std::lock_guard lock(alias_models_mutex_);
      ReconcileCachedAlias(*c);
    });
    container->SelectDefaultVariant(MakeVariantEligibility());
    if (container->HasEligibleDefaultVariant()) {
      return alias_models_.emplace(alias, std::move(container)).first->second.get();
    }
    return nullptr;
  };
  if (auto* cached = make_alias(CachedVariants(alias))) return cached;

  if (cache_only_) {
    return nullptr;
  }

  // Use the shared listing refresh interval rather than enumerating the registry for every alias miss.
  std::vector<ModelInfo> infos;
  for (const auto* group : RemoteModelCatalog::ListModels()) {
    if (group->Alias() == alias) {
      for (const auto* variant : group->Variants()) infos.push_back(variant->Info());
    }
  }
  return make_alias(CreateModels(infos, Query::Listing));
}

Model* AzureRegistryCatalog::GetModelVariant(const std::string& model_id) const {
  if (model_id.find(':') == std::string::npos) return RemoteModelCatalog::GetModelVariant(model_id);

  {
    std::lock_guard lock(exact_models_mutex_);
    if (const auto found = exact_models_.find(model_id); found != exact_models_.end()) {
      RefreshRetainedVariantCache(*found->second);
      return found->second.get();
    }
  }

  const auto indexed = FindIndexedModelInfo(model_id);
  auto models = indexed ? CreateModels({*indexed}) : FetchModelsByIds({model_id});
  if (models.empty()) {
    logger_.Log(LogLevel::Information, "GetModelVariant: '" + model_id + "' not found in the Azure ML catalog.");
    return nullptr;
  }
  auto model = std::make_unique<Model>(std::move(models.front()));
  std::lock_guard lock(exact_models_mutex_);
  return exact_models_.emplace(model_id, std::move(model)).first->second.get();
}

Model* AzureRegistryCatalog::GetLatestVersion(const Model* model) const {
  if (!model) return nullptr;
  if (!cache_only_) {
    EnsurePopulated(/*allow_refresh=*/true);
  }
  return RemoteModelCatalog::GetLatestVersion(model);
}

std::vector<Model*> AzureRegistryCatalog::GetCachedModels() const {
  std::vector<std::string> completed_ids;
  {
    std::lock_guard snapshot_lock(snapshot_mutex_);
    CatalogCache cache(cache_dir_, logger_);
    cache.Load();
    if (const auto infos = cache.GetCachedModels()) {
      for (const auto& info : *infos) {
        if (!downloader_->GetCompletedModelCachePath(info).empty()) {
          completed_ids.push_back(info.model_id);
        }
      }
    }
  }
  for (const auto& id : completed_ids) {
    GetModelVariant(id);
  }

  std::vector<Model*> models = RemoteModelCatalog::GetCachedModels();
  std::unordered_set<std::string> seen_ids;
  for (auto* m : models) seen_ids.insert(m->Id());

  const auto add_if_cached = [&](Model* m) {
    if (m) {
      RefreshRetainedVariantCache(*m);
      if (m->IsCached() && seen_ids.insert(m->Id()).second) {
        models.push_back(m);
      }
    }
  };

  for (auto* variant : RetainedVariants()) add_if_cached(variant);
  return models;
}

std::vector<Model*> AzureRegistryCatalog::GetLoadedModels() const {
  std::vector<Model*> models = RemoteModelCatalog::GetLoadedModels();
  std::unordered_set<std::string> seen_ids;
  for (auto* m : models) seen_ids.insert(m->Id());

  const auto add_if_loaded = [&](Model* m) {
    if (m && m->IsLoaded() && seen_ids.insert(m->Id()).second) {
      models.push_back(m);
    }
  };

  for (auto* variant : RetainedVariants()) add_if_loaded(variant);
  return models;
}

std::vector<Model*> AzureRegistryCatalog::RetainedVariants() const {
  std::vector<Model*> models;
  {
    std::lock_guard lock(exact_models_mutex_);
    for (const auto& [id, model] : exact_models_) {
      models.push_back(model.get());
    }
  }
  {
    std::lock_guard lock(alias_models_mutex_);
    for (const auto& [alias, container] : alias_models_) {
      for (auto* variant : container->Variants()) {
        models.push_back(variant);
      }
    }
  }
  return models;
}

std::string AzureRegistryCatalog::GetModelPath(const ModelInfo& info, Query) const {
  const auto* registry = info.GetPropertyStr(kRegistryProperty);
  if (!registry || *registry != registry_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Azure ML catalog cache contains another registry's metadata");
  }
  return downloader_->GetCompletedModelCachePath(info);
}

std::vector<ModelInfo> AzureRegistryCatalog::FetchModelInfos() const {
  std::lock_guard snapshot_lock(snapshot_mutex_);
  FileLock transaction(std::filesystem::path(cache_dir_) / "foundry.modelinfo.lock");
  CatalogCache cache(cache_dir_, logger_);
  std::vector<ModelInfo> live_infos;
  if (cache_only_) {
    cache.Load();
    const auto cached = cache.GetCachedModels();
    if (!cached) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "no Azure ML catalog snapshot; run online with this registry and cache first");
    }
    live_infos = *cached;
  } else {
    for (const auto& model : Client().ListModelVersions()) {
      // One malformed opted-in asset must not hide the rest of the registry; exact-ID lookups still fail explicitly.
      try {
        if (auto info = Adapt(model)) live_infos.push_back(std::move(*info));
      } catch (const nlohmann::json::exception& error) {
        logger_.Log(LogLevel::Warning, std::string("Skipping malformed Azure ML model metadata: ") + error.what());
      } catch (const Exception& error) {
        if (error.code() != FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT) throw;
        logger_.Log(LogLevel::Warning, std::string("Skipping invalid Azure ML model metadata: ") + error.what());
      }
    }
    cache.Load();
    auto snapshot_infos = live_infos;
    if (const auto existing = cache.GetCachedModels()) {
      for (const auto& old_info : *existing) {
        const auto* resource_id = old_info.GetPropertyStr("azureml.registryResourceId");
        if (resource_id && *resource_id != Client().ResourceId()) {
          continue;
        }
        if (std::none_of(snapshot_infos.begin(), snapshot_infos.end(), [&](const ModelInfo& m) {
              return m.model_id == old_info.model_id;
            })) {
          if (!downloader_->GetCompletedModelCachePath(old_info).empty()) {
            snapshot_infos.push_back(old_info);
          }
        }
      }
    }
    cache.Save(snapshot_infos, true);
  }
  if (cache_only_) {
    std::erase_if(live_infos, [&](const ModelInfo& info) { return GetModelPath(info, Query::Listing).empty(); });
  }
  return live_infos;
}

std::optional<std::vector<ModelInfo>> AzureRegistryCatalog::FetchLookupModelInfos() const {
  std::lock_guard snapshot_lock(snapshot_mutex_);
  CatalogCache cache(cache_dir_, logger_);
  cache.Load();
  auto infos = cache.GetCachedModels();
  if (!infos && cache_only_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "no Azure ML catalog snapshot; run online with this registry and cache first");
  }
  auto models = infos.value_or(std::vector<ModelInfo>{});
  std::erase_if(models, [&](const ModelInfo& info) { return GetModelPath(info, Query::Lookup).empty(); });
  return models;
}

std::vector<ModelInfo> AzureRegistryCatalog::FetchModelVersionInfos(const std::string& alias, const std::string& name,
                                                                  int /*max_versions*/) const {
  std::vector<ModelInfo> models;
  if (name.empty() || cache_only_) {
    models = FetchModelInfos();
  } else {
    std::lock_guard snapshot_lock(snapshot_mutex_);
    FileLock transaction(std::filesystem::path(cache_dir_) / "foundry.modelinfo.lock");
    for (const auto& model : Client().ListModelVersions(name)) {
      if (auto info = Adapt(model)) models.push_back(std::move(*info));
    }
    CatalogCache cache(cache_dir_, logger_);
    cache.Load();
    auto snapshot = cache.GetCachedModels().value_or(std::vector<ModelInfo>{});
    for (const auto& info : models) {
      std::erase_if(snapshot, [&](const ModelInfo& cached) { return cached.model_id == info.model_id; });
      snapshot.push_back(info);
    }
    cache.Save(snapshot, true);
  }
  std::erase_if(models, [&](const ModelInfo& model) {
    return model.alias != alias || (!name.empty() && model.name != name);
  });
  return models;
}

std::vector<ModelInfo> AzureRegistryCatalog::FetchModelInfosByIds(const std::vector<std::string>& ids) const {
  std::lock_guard snapshot_lock(snapshot_mutex_);
  FileLock transaction(std::filesystem::path(cache_dir_) / "foundry.modelinfo.lock");
  CatalogCache cache(cache_dir_, logger_);
  cache.Load();
  auto snapshot = cache.GetCachedModels().value_or(std::vector<ModelInfo>{});
  std::vector<ModelInfo> infos;
  for (const auto& id : ids) {
    const auto [name, version] = Utils::SplitModelNameAndVersion(id);
    if (version <= 0) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Azure ML model ID must use <variant-name>:<positive-version>");
    }
    const auto cached = std::find_if(snapshot.begin(), snapshot.end(), [&](const ModelInfo& info) {
      return info.model_id == id && !downloader_->GetCompletedModelCachePath(info).empty();
    });
    if (cached != snapshot.end()) {
      infos.push_back(*cached);
    } else if (!cache_only_) {
      if (const auto model = Client().GetModelVersion(name, std::to_string(version))) {
        if (auto info = Adapt(*model)) infos.push_back(std::move(*info));
      }
    }
  }
  for (const auto& info : infos) {
    std::erase_if(snapshot, [&](const ModelInfo& cached) { return cached.model_id == info.model_id; });
    snapshot.push_back(info);
  }
  if (!cache_only_) cache.Save(snapshot, true);
  return infos;
}

void AzureRegistryCatalog::RefreshRetainedVariantCache(Model& model) const {
  model.UpdateCachedState(downloader_->GetCompletedModelCachePath(model.Info()));
}

}  // namespace fl
