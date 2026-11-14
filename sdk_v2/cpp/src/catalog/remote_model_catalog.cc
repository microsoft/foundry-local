// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/remote_model_catalog.h"

#include "catalog/azure_catalog_client.h"
#include "catalog/capacity_compatibility.h"
#include "inferencing/execution_provider.h"
#include "inferencing/generative/genai_config.h"
#include "version.h"

#include <algorithm>
#include <filesystem>
#include <fmt/format.h>
#include <unordered_map>
#include <unordered_set>

namespace fl {
namespace {

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

}  // namespace

class RemoteModelCatalog::Devices final : public IEpDetector {
 public:
  explicit Devices(const IEpDetector& local) : local_(local) {}

  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    std::lock_guard lock(mutex_);
    return hosted_ ? *hosted_ : local_.GetAvailableDevicesToEPs();
  }

  std::map<std::string, std::vector<DeviceCapacity>> GetDeviceCapacities() const override {
    std::lock_guard lock(mutex_);
    if (!hosted_) return local_.GetDeviceCapacities();

    std::map<std::string, std::vector<DeviceCapacity>> capacities;
    for (const auto& target : execution_targets_) {
      capacities[target.provider].push_back(target.capacity);
    }

    return capacities;
  }

  std::vector<ExecutionTarget> GetExecutionTargets() const override {
    std::lock_guard lock(mutex_);
    if (!hosted_) return local_.GetExecutionTargets();

    auto targets = execution_targets_;
    for (const auto& [type, providers] : *hosted_) {
      for (const auto& provider : providers) {
        if (std::none_of(targets.begin(), targets.end(), [&](const auto& target) {
              return ToLower(EPUtils::NormalizeRegistrationName(target.provider)) ==
                     ToLower(EPUtils::NormalizeRegistrationName(provider));
            })) {
          targets.push_back({provider, 0, type, {}});
        }
      }
    }

    return targets;
  }

  bool Set(std::map<std::string, std::vector<std::string>> devices,
           std::vector<ExecutionTarget> execution_targets) {
    std::lock_guard lock(mutex_);
    if (hosted_ && *hosted_ == devices && execution_targets_ == execution_targets) return false;
    hosted_ = std::move(devices);
    execution_targets_ = std::move(execution_targets);
    return true;
  }

 private:
  const IEpDetector& local_;
  mutable std::mutex mutex_;
  std::optional<std::map<std::string, std::vector<std::string>>> hosted_;
  std::vector<ExecutionTarget> execution_targets_;
};

RemoteModelCatalog::RemoteModelCatalog(std::string name, CatalogType type, ModelFactory factory,
                                       const IEpDetector& ep_detector, ILogger& logger)
    : BaseModelCatalog(std::move(name), type, logger),
      devices_(std::make_unique<Devices>(ep_detector)), factory_(std::move(factory)), logger_(logger) {}

RemoteModelCatalog::~RemoteModelCatalog() = default;

void RemoteModelCatalog::SetRuntimeDevices(std::map<std::string, std::vector<std::string>> devices,
                                          std::vector<ExecutionTarget> execution_targets) {
  if (devices_->Set(std::move(devices), std::move(execution_targets))) InvalidateCache();
}

const IEpDetector& RemoteModelCatalog::CatalogDevices() const { return *devices_; }

bool RemoteModelCatalog::IsModelCompatible(const ModelInfo& info) const {
  const auto* minimum = info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR);
  if (minimum && !minimum->empty() && !IsFoundryLocalVersionCompatible(FOUNDRY_LOCAL_VERSION, *minimum)) return false;
  const auto* os = info.GetPropertyStr("os");
  const auto* architecture = info.GetPropertyStr("architecture");
  return (!os || *os == HostOS()) && (!architecture || *architecture == HostArchitecture());
}

std::vector<Model> RemoteModelCatalog::CreateModels(const std::vector<ModelInfo>& infos, Query query) const {
  std::vector<Model> models;
  models.reserve(infos.size());
  const auto eligible = GetType() == CatalogType::kAzureRegistry &&
      (query == Query::Listing || query == Query::History)
      ? MakeCompatibilityPredicate(false, query != Query::Listing) : VariantEligibility{};
  const auto visible = query == Query::History ? MakeVariantVisibility() : VariantEligibility{};
  for (const auto& info : infos) {
    if (!IsModelCompatible(info)) continue;

    auto path = GetModelPath(info, query);
    if (RequiresCompletedCache() && path.empty()) continue;
    auto model = factory_(info, std::move(path));
    if (eligible && !eligible(model)) continue;
    if (visible && !visible(model)) continue;
    models.push_back(std::move(model));
  }
  return models;
}

std::vector<Model> RemoteModelCatalog::FetchModels() const {
  auto models = CreateModels(FetchModelInfos(), Query::Listing);
  if (GetType() == CatalogType::kAzureRegistry) {
    std::map<std::string, int> latest;
    for (const auto& model : models) latest[model.Info().name] = std::max(latest[model.Info().name], model.Info().version);
    std::erase_if(models, [&](const Model& model) { return model.Info().version < latest.at(model.Info().name); });
  }
  return models;
}

std::optional<std::vector<Model>> RemoteModelCatalog::FetchLookupModels() const {
  const auto infos = FetchLookupModelInfos();
  return infos ? std::optional(CreateModels(*infos, Query::Lookup)) : std::nullopt;
}

std::vector<Model> RemoteModelCatalog::FetchModelsByIds(const std::vector<std::string>& ids) const {
  return CreateModels(FetchModelInfosByIds(ids), Query::Exact);
}

std::vector<Model> RemoteModelCatalog::FetchModelVersions(const std::string& alias, const std::string& name,
                                                         int max_versions) const {
  auto models = CreateModels(FetchModelVersionInfos(alias, name, max_versions), Query::History);
  if (GetType() == CatalogType::kAzureRegistry) {
    std::sort(models.begin(), models.end(), Model::CompareBestFirst);
    std::map<std::string, int> counts;
    if (max_versions > 0) {
      std::erase_if(models, [&](const Model& model) { return ++counts[model.Info().name] > max_versions; });
    }
  }
  return models;
}

BaseModelCatalog::VariantEligibility RemoteModelCatalog::MakeVariantEligibility() const {
  return MakeCompatibilityPredicate(false);
}

BaseModelCatalog::VariantEligibility RemoteModelCatalog::MakeVariantVisibility() const {
  return [this, compatible = VariantEligibility{}](const Model& variant) mutable {
    if (variant.Info().device_type != DeviceType::kGPU ||
        !variant.Info().GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT)) {
      return true;
    }
    if (!compatible) compatible = MakeCompatibilityPredicate(true);
    return compatible(variant);
  };
}

BaseModelCatalog::VariantEligibility RemoteModelCatalog::MakeCompatibilityPredicate(bool capacity_only,
                                                                                   bool check_capacity) const {
  auto targets = devices_->GetExecutionTargets();
  auto devices = !capacity_only && GetType() == CatalogType::kAzureRegistry
      ? devices_->GetAvailableDevicesToEPs() : std::map<std::string, std::vector<std::string>>{};
  std::unordered_set<std::string> providers;
  for (const auto& target : targets) {
    providers.insert(ToLower(EPUtils::NormalizeRegistrationName(target.provider)));
  }

  return [this, capacity_only, check_capacity, devices = std::move(devices), targets = std::move(targets),
          providers = std::move(providers),
          configured_models = std::unordered_map<std::string, std::optional<GenAIConfig>>{}](
             const Model& variant) mutable {
    const auto& info = variant.Info();
    if (!capacity_only && !IsModelCompatible(info)) return false;
    ModelCapacityRequirements requirements;
    if (const auto* bytes = info.GetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_MIN_VRAM_BYTES_INT); bytes && *bytes >= 0) {
      requirements.minimum_vram_bytes = static_cast<uint64_t>(*bytes);
    }
    if (capacity_only && (info.device_type != DeviceType::kGPU || !requirements.minimum_vram_bytes)) return true;

    const bool generic_gpu = info.model_id.find("generic-gpu") != std::string::npos;
    bool is_multimodal = HasNonTextModalities(info);
    auto provider = info.execution_provider;
    if (generic_gpu || provider.empty()) {
      const auto path = variant.LocalPathSnapshot();
      if (!path.empty()) {
        auto [entry, inserted] = configured_models.try_emplace(path);
        if (inserted) {
          try {
            entry->second = GenAIConfig::LoadFromFile((std::filesystem::path(path) / "genai_config.json").string());
          } catch (const std::exception& ex) {
            logger_.Log(LogLevel::Warning, fmt::format("Cannot determine execution provider for model '{}': {}",
                                                      info.model_id, ex.what()));
          }
        }
        if (entry->second) {
          is_multimodal = entry->second->model && entry->second->model->IsMultiModal();
          if (provider.empty() || (generic_gpu && is_multimodal)) {
            provider = entry->second->DefaultProvider();
          }
        } else if (provider.empty() && !generic_gpu) {
          return capacity_only;
        }
      }
    }
    if (provider.empty()) provider = EPUtils::RequiredEpForModelId(info.model_id);
    provider = ToLower(EPUtils::NormalizeRegistrationName(provider));

    const auto meets_capacity = [&](const std::string& provider, bool generic_gpu = false) {
      return !check_capacity || EvaluateProviderCapacity(info.device_type == DeviceType::kGPU, requirements, targets,
                                                         provider, generic_gpu, is_multimodal)
                                        .state != CompatibilityState::kDoesNotFit;
    };
    if (!capacity_only && GetType() == CatalogType::kAzureRegistry) {
      const auto device = devices.find(DeviceTypeToString(info.device_type));
      if (device == devices.end()) return false;
      return std::any_of(device->second.begin(), device->second.end(), [&](const std::string& ep) {
               const auto available = ToLower(EPUtils::NormalizeRegistrationName(ep));
               return available == provider ||
                      (generic_gpu && (available == "webgpuexecutionprovider" ||
                                       (!is_multimodal && available == "cudaexecutionprovider")));
             }) &&
             meets_capacity(provider, generic_gpu);
    }
    if (generic_gpu && (providers.contains("webgpuexecutionprovider") ||
                        (!is_multimodal && providers.contains("cudaexecutionprovider")))) {
      return meets_capacity(provider, true);
    }
    return (capacity_only || provider.empty() || provider == "cpuexecutionprovider" || providers.contains(provider)) &&
           meets_capacity(provider);
  };
}

}  // namespace fl
