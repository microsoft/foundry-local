// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/remote_model_catalog.h"

#include "catalog/azure_catalog_client.h"
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

  bool Set(std::map<std::string, std::vector<std::string>> devices) {
    std::lock_guard lock(mutex_);
    if (hosted_ && *hosted_ == devices) return false;
    hosted_ = std::move(devices);
    return true;
  }

 private:
  const IEpDetector& local_;
  mutable std::mutex mutex_;
  std::optional<std::map<std::string, std::vector<std::string>>> hosted_;
};

RemoteModelCatalog::RemoteModelCatalog(std::string name, CatalogType type, ModelFactory factory,
                                       const IEpDetector& ep_detector, ILogger& logger)
    : BaseModelCatalog(std::move(name), type, logger),
      devices_(std::make_unique<Devices>(ep_detector)), factory_(std::move(factory)), logger_(logger) {}

RemoteModelCatalog::~RemoteModelCatalog() = default;

void RemoteModelCatalog::SetRuntimeDevices(std::map<std::string, std::vector<std::string>> devices) {
  if (devices_->Set(std::move(devices))) InvalidateCache();
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
      (query == Query::Listing || query == Query::History) ? MakeVariantEligibility() : VariantEligibility{};
  for (const auto& info : infos) {
    if (!IsModelCompatible(info)) continue;

    auto path = GetModelPath(info, query);
    if (RequiresCompletedCache() && path.empty()) continue;
    auto model = factory_(info, std::move(path));
    if (eligible && !eligible(model)) continue;
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
  auto devices = devices_->GetAvailableDevicesToEPs();
  std::unordered_set<std::string> providers;
  for (const auto& [device, eps] : devices) {
    for (const auto& ep : eps) providers.insert(ToLower(EPUtils::NormalizeRegistrationName(ep)));
  }

  return [this, devices = std::move(devices), providers = std::move(providers),
          configured_providers = std::unordered_map<std::string, std::optional<std::string>>{}](
             const Model& variant) mutable {
    const auto& info = variant.Info();
    if (!IsModelCompatible(info)) return false;
    const bool generic_gpu = info.model_id.find("generic-gpu") != std::string::npos;
    if (GetType() == CatalogType::kAzureRegistry) {
      const auto device = devices.find(DeviceTypeToString(info.device_type));
      if (device == devices.end()) return false;
      const auto provider = ToLower(EPUtils::NormalizeRegistrationName(info.execution_provider));
      return std::any_of(device->second.begin(), device->second.end(), [&](const std::string& ep) {
        const auto available = ToLower(EPUtils::NormalizeRegistrationName(ep));
        return available == provider ||
            (generic_gpu && (available == "cudaexecutionprovider" || available == "webgpuexecutionprovider"));
      });
    }
    if (generic_gpu && (providers.contains("cudaexecutionprovider") || providers.contains("webgpuexecutionprovider"))) {
      return true;
    }
    auto provider = info.execution_provider;
    if (provider.empty()) {
      const auto path = variant.LocalPathSnapshot();
      if (!path.empty()) {
        auto [entry, inserted] = configured_providers.try_emplace(path);
        if (inserted) {
          try {
            entry->second = GenAIConfig::LoadFromFile((std::filesystem::path(path) / "genai_config.json").string())
                                .DefaultProvider();
          } catch (const std::exception& ex) {
            logger_.Log(LogLevel::Warning, fmt::format("Cannot determine execution provider for model '{}': {}",
                                                      info.model_id, ex.what()));
          }
        }
        if (!entry->second) return false;
        provider = *entry->second;
      }
    }
    if (provider.empty()) provider = EPUtils::RequiredEpForModelId(info.model_id);
    provider = ToLower(EPUtils::NormalizeRegistrationName(provider));
    return provider.empty() || provider == "cpuexecutionprovider" || providers.contains(provider);
  };
}

}  // namespace fl
