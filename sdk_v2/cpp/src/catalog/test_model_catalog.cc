// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/test_model_catalog.h"

#include "catalog/azure_catalog_models.h"
#include "catalog/catalog_cache.h"
#include "inferencing/execution_provider.h"
#include "telemetry/telemetry_environment.h"
#include "util/sha256.h"
#include "utils.h"

#include <azure/identity/azure_cli_credential.hpp>
#include <azure/identity/workload_identity_credential.hpp>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <map>
#include <unordered_set>

namespace fl {
namespace {

std::string Setting(const Configuration& config, const char* key, const char* fallback) {
  const auto it = config.additional_options.find(key);
  return it == config.additional_options.end() ? fallback : it->second;
}

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
           "Set TestModelCredential to azure_cli or workload_identity; CI must explicitly select workload_identity.");
}

const char* HostOS() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__ANDROID__)
  return "android";
#else
  return "linux";
#endif
}

const char* HostArchitecture() {
#if defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
  return "x64";
#else
  return "unsupported";
#endif
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

TestModelCatalog::TestModelCatalog(std::string registry, const Configuration& config,
                                   const IEpDetector& ep_detector, ModelLoadManager& load_manager,
                                   ILogger& logger, ITelemetry& telemetry,
                                   std::unique_ptr<AzureMlRegistryClient> client)
    : AzureModelCatalog({}, CacheDirectory(registry, config),
          [this](ModelInfo info, std::string path) {
            return Model::FromModelInfo(std::move(info), std::move(path), *downloader_, load_manager_);
          }, ep_detector, logger, false, telemetry, CatalogType::kTest),
      registry_(std::move(registry)),
      cache_dir_(CacheDirectory(registry_, config)),
      cache_only_(Setting(config, "TestModelCacheOnly", "false") == "true"),
      credential_mode_(Setting(config, "TestModelCredential", "")),
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

AzureMlRegistryClient& TestModelCatalog::Client() const {
  if (cache_only_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot access the registry in cache-only mode");
  }
  std::lock_guard lock(client_mutex_);
  if (!client_) client_ = std::make_unique<AzureMlRegistryClient>(registry_, Credential(credential_mode_));
  return *client_;
}

std::optional<ModelInfo> TestModelCatalog::Adapt(const nlohmann::json& model) const {
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
  CatalogLocalModel normalized;
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
    if (os != HostOS() || architecture != HostArchitecture()) return std::nullopt;
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
  normalized.variant_information = VariantInformation{{}, variant};
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
  info->string_properties["test.registryResourceId"] = Client().ResourceId();
  return info;
}

std::vector<Model*> TestModelCatalog::ListModels() const {
  auto models = AzureModelCatalog::ListModels();
  for (auto* model : models) {
    for (auto* variant : model->Variants()) RefreshRetainedVariantCache(*variant);
  }
  return models;
}

Model* TestModelCatalog::GetModel(const std::string& alias) const {
  std::lock_guard lock(alias_models_mutex_);
  auto it = alias_models_.find(alias);
  if (it != alias_models_.end()) {
    for (auto* variant : it->second->Variants()) {
      RefreshRetainedVariantCache(*variant);
    }
    it->second->SelectDefaultVariant(MakeVariantEligibility(), /*preserve_explicit_selection=*/true);
    if (it->second->HasEligibleDefaultVariant()) {
      return it->second.get();
    }
    return nullptr;
  }

  // First, check completed packages in cache for this alias (cache-first alias behavior).
  std::vector<Model> cached_variants;
  {
    std::lock_guard snapshot_lock(snapshot_mutex_);
    CatalogCache cache(cache_dir_, logger_);
    cache.Load();
    if (const auto infos = cache.GetCachedModels()) {
      for (const auto& info : *infos) {
        if (info.alias == alias) {
          const auto path = downloader_->GetCompletedModelCachePath(info);
          if (!path.empty()) {
            cached_variants.push_back(Model::FromModelInfo(info, path, *downloader_, load_manager_));
          }
        }
      }
    }
  }

  if (!cached_variants.empty()) {
    std::sort(cached_variants.begin(), cached_variants.end(), Model::CompareBestFirst);
    auto container = std::make_unique<Model>(Model::MakeContainer(std::move(cached_variants.front())));
    for (size_t i = 1; i < cached_variants.size(); ++i) {
      container->AddVariant(std::move(cached_variants[i]));
    }
    container->SetDefaultVariantRefresh([this, c = container.get()] {
      EnsurePopulated();
      c->SelectDefaultVariant(MakeVariantEligibility(), /*preserve_explicit_selection=*/true);
    });
    container->SelectDefaultVariant(MakeVariantEligibility());
    if (container->HasEligibleDefaultVariant()) {
      return alias_models_.emplace(alias, std::move(container)).first->second.get();
    }
    if (cache_only_) {
      return nullptr;
    }
  }

  if (cache_only_) {
    return nullptr;
  }

  // Cache miss: query the registry for this alias.
  auto fetched = FetchModels();
  std::erase_if(fetched, [&](const Model& model) { return model.Alias() != alias; });
  if (fetched.empty()) {
    return nullptr;
  }

  auto container = std::make_unique<Model>(Model::MakeContainer(std::move(fetched.front())));
  for (size_t i = 1; i < fetched.size(); ++i) {
    container->AddVariant(std::move(fetched[i]));
  }
  container->SetDefaultVariantRefresh([this, c = container.get()] {
    EnsurePopulated();
    c->SelectDefaultVariant(MakeVariantEligibility(), /*preserve_explicit_selection=*/true);
  });
  container->SelectDefaultVariant(MakeVariantEligibility());
  if (!container->HasEligibleDefaultVariant()) {
    return nullptr;
  }
  return alias_models_.emplace(alias, std::move(container)).first->second.get();
}

Model* TestModelCatalog::GetModelVariant(const std::string& model_id) const {
  if (model_id.find(':') == std::string::npos) return AzureModelCatalog::GetModelVariant(model_id);

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
    logger_.Log(LogLevel::Information, "GetModelVariant: '" + model_id + "' not found in the test catalog.");
    return nullptr;
  }
  auto model = std::make_unique<Model>(std::move(models.front()));
  std::lock_guard lock(exact_models_mutex_);
  return exact_models_.emplace(model_id, std::move(model)).first->second.get();
}

Model* TestModelCatalog::GetLatestVersion(const Model* model) const {
  if (!model) return nullptr;
  if (!cache_only_) {
    EnsurePopulated(/*allow_refresh=*/true);
  }
  return AzureModelCatalog::GetLatestVersion(model);
}

std::vector<Model*> TestModelCatalog::GetCachedModels() const {
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

  std::vector<Model*> models = AzureModelCatalog::GetCachedModels();
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

  {
    std::lock_guard lock(exact_models_mutex_);
    for (const auto& [id, model] : exact_models_) {
      add_if_cached(model.get());
    }
  }
  {
    std::lock_guard lock(alias_models_mutex_);
    for (const auto& [alias, container] : alias_models_) {
      for (auto* variant : container->Variants()) {
        add_if_cached(variant);
      }
    }
  }
  return models;
}

std::vector<Model*> TestModelCatalog::GetLoadedModels() const {
  std::vector<Model*> models = AzureModelCatalog::GetLoadedModels();
  std::unordered_set<std::string> seen_ids;
  for (auto* m : models) seen_ids.insert(m->Id());

  const auto add_if_loaded = [&](Model* m) {
    if (m && m->IsLoaded() && seen_ids.insert(m->Id()).second) {
      models.push_back(m);
    }
  };

  {
    std::lock_guard lock(exact_models_mutex_);
    for (const auto& [id, model] : exact_models_) {
      add_if_loaded(model.get());
    }
  }
  {
    std::lock_guard lock(alias_models_mutex_);
    for (const auto& [alias, container] : alias_models_) {
      for (auto* variant : container->Variants()) {
        add_if_loaded(variant);
      }
    }
  }
  return models;
}

std::vector<Model> TestModelCatalog::CreateModels(const std::vector<ModelInfo>& infos) const {
  std::vector<Model> models;
  for (const auto& info : infos) {
    const auto* registry = info.GetPropertyStr(kRegistryProperty);
    if (!registry || *registry != registry_) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "test catalog cache contains another registry's metadata");
    }
    const auto path = downloader_->GetCompletedModelCachePath(info);
    if (!cache_only_ || !path.empty()) {
      models.push_back(Model::FromModelInfo(info, path, *downloader_, load_manager_));
    }
  }
  return models;
}

std::vector<Model> TestModelCatalog::FetchModels() const {
  auto models = FetchAvailableModels();
  std::map<std::string, int> latest_versions;
  for (const auto& model : models) {
    const auto& info = model.Info();
    auto& latest = latest_versions[info.name];
    latest = std::max(latest, info.version);
  }
  std::erase_if(models, [&](const Model& model) {
    return model.Info().version < latest_versions.at(model.Info().name);
  });
  return models;
}

std::vector<Model> TestModelCatalog::FetchAvailableModels() const {
  std::lock_guard snapshot_lock(snapshot_mutex_);
  CatalogCache cache(cache_dir_, logger_);
  std::vector<ModelInfo> live_infos;
  if (cache_only_) {
    cache.Load();
    const auto cached = cache.GetCachedModels();
    if (!cached) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "no test catalog snapshot; run online with this registry and cache first");
    }
    live_infos = *cached;
  } else {
    for (const auto& model : Client().ListModelVersions()) {
      try {
        if (auto info = Adapt(model)) live_infos.push_back(std::move(*info));
      } catch (const nlohmann::json::exception&) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Azure ML returned malformed model metadata");
      }
    }
    cache.Load();
    auto snapshot_infos = live_infos;
    if (const auto existing = cache.GetCachedModels()) {
      for (const auto& old_info : *existing) {
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
  auto models = CreateModels(live_infos);
  const auto eligible = MakeVariantEligibility();
  std::erase_if(models, [&](const Model& model) { return !eligible(model); });
  return models;
}

BaseModelCatalog::VariantEligibility TestModelCatalog::MakeVariantEligibility() const {
  return [devices = AvailableDevicesToEPs(), eligible = AzureModelCatalog::MakeVariantEligibility()](
             const Model& model) {
    const auto& info = model.Info();
    const auto device = devices.find(DeviceTypeToString(info.device_type));
    if (device == devices.end() || device->second.empty() || !eligible(model)) return false;
    const bool generic_gpu = info.model_id.find("-generic-gpu:") != std::string::npos;
    const auto provider = ToLower(EPUtils::NormalizeRegistrationName(info.execution_provider));
    return std::any_of(device->second.begin(), device->second.end(), [&](const std::string& ep) {
      const auto available = ToLower(EPUtils::NormalizeRegistrationName(ep));
      return available == provider ||
          (generic_gpu && (available == "cudaexecutionprovider" || available == "webgpuexecutionprovider"));
    });
  };
}

std::optional<std::vector<Model>> TestModelCatalog::FetchLookupModels() const {
  std::lock_guard snapshot_lock(snapshot_mutex_);
  CatalogCache cache(cache_dir_, logger_);
  cache.Load();
  const auto infos = cache.GetCachedModels();
  if (!infos && cache_only_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "no test catalog snapshot; run online with this registry and cache first");
  }
  auto models = infos ? CreateModels(*infos) : std::vector<Model>{};
  std::erase_if(models, [](const Model& model) { return model.LocalPathSnapshot().empty(); });
  return models;
}

std::vector<Model> TestModelCatalog::FetchModelVersions(const std::string& alias, const std::string& name,
                                                       int max_versions) const {
  auto models = FetchAvailableModels();
  std::erase_if(models, [&](const Model& model) {
    return model.Alias() != alias || (!name.empty() && model.Info().name != name);
  });
  std::sort(models.begin(), models.end(), Model::CompareBestFirst);
  std::map<std::string, int> counts;
  if (max_versions > 0) {
    std::erase_if(models, [&](const Model& model) { return ++counts[model.Info().name] > max_versions; });
  }
  return models;
}

std::vector<Model> TestModelCatalog::FetchModelsByIds(const std::vector<std::string>& ids) const {
  std::lock_guard snapshot_lock(snapshot_mutex_);
  CatalogCache cache(cache_dir_, logger_);
  cache.Load();
  auto snapshot = cache.GetCachedModels().value_or(std::vector<ModelInfo>{});
  std::vector<ModelInfo> infos;
  for (const auto& id : ids) {
    const auto [name, version] = Utils::SplitModelNameAndVersion(id);
    if (version <= 0) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "test model ID must use <variant-name>:<positive-version>");
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
  return CreateModels(infos);
}

void TestModelCatalog::RefreshRetainedVariantCache(Model& model) const {
  model.UpdateCachedState(downloader_->GetCompletedModelCachePath(model.Info()));
}

}  // namespace fl
