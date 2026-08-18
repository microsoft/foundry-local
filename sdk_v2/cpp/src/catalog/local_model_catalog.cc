// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/local_model_catalog.h"

#include "exception.h"
#include "inferencing/generative/genai_config.h"
#include "util/file_lock.h"
#include "util/time_utils.h"

#include <foundry_local/foundry_local_c.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <regex>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fl {
namespace {

constexpr const char* kRegistrationIdProperty = "_local_registration_id";
constexpr const char* kLegacyModelPathProperty = "model_path";
constexpr const char* kLegacyAliasProperty = "alias";
constexpr const char* kLegacyVersionProperty = "version";
const std::regex kModelNamePattern("[A-Za-z0-9][A-Za-z0-9._-]*");
std::mutex kLocalCatalogMutationMutex;
const std::unordered_set<std::string> kSupportedTasks = {
    "automatic-speech-recognition",
    "chat-completion",
    "embeddings",
    "vision-language-chat",
};

bool HasParentTraversal(const std::filesystem::path& path) {
  return std::any_of(path.begin(), path.end(), [](const auto& component) { return component == ".."; });
}

struct ParsedModelId {
  std::string name;
  int version;
};

ParsedModelId ParseModelId(const std::string& model_id) {
  const auto separator = model_id.find(':');
  if (separator == std::string::npos || separator == 0 || separator + 1 == model_id.size() ||
      model_id.find(':', separator + 1) != std::string::npos) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model_id must have format <name>:<version>");
  }

  auto name = model_id.substr(0, separator);
  if (!std::regex_match(name, kModelNamePattern)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model_id name must match [a-zA-Z0-9][a-zA-Z0-9._-]*");
  }

  const auto version_text = model_id.substr(separator + 1);
  if ((version_text.size() > 1 && version_text.front() == '0') ||
      !std::all_of(version_text.begin(), version_text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model_id version must be a canonical non-negative integer");
  }

  int version = 0;
  const auto [end, error] = std::from_chars(version_text.data(), version_text.data() + version_text.size(), version);
  if (error != std::errc{} || end != version_text.data() + version_text.size()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model_id version is out of range");
  }

  return {std::move(name), version};
}

void RemoveLegacyRegistrationProperties(ModelInfo& info) {
  info.string_properties.erase(kLegacyModelPathProperty);
  info.string_properties.erase(kLegacyAliasProperty);
  info.string_properties.erase(kRegistrationIdProperty);
  info.string_properties.erase(kLegacyVersionProperty);
  info.int_properties.erase(kLegacyModelPathProperty);
  info.int_properties.erase(kLegacyAliasProperty);
  info.int_properties.erase(kRegistrationIdProperty);
  info.int_properties.erase(kLegacyVersionProperty);
}

nlohmann::json RegistrationToJson(const LocalModelCatalog::Registration& registration) {
  return {
      {"model_id", registration.info.model_id},
      {"model_path", registration.model_path},
      {"registration_id", registration.registration_id},
      {"registered_at",
       registration.info.GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CREATION_TIME_STR, std::string{})},
      {"properties", ModelInfoToPropertyBagJson(registration.info)},
  };
}

}  // namespace

LocalModelCatalog::LocalModelCatalog(std::filesystem::path app_data_dir, ModelFactory model_factory, ILogger& logger)
    : BaseModelCatalog("local", CatalogType::kLocal, logger),
      catalog_dir_(std::move(app_data_dir) / "catalogs" / "local"),
      index_path_(catalog_dir_ / "local_models.json"),
      lock_path_(catalog_dir_ / "local_models.lock"),
      model_factory_(std::move(model_factory)),
      logger_(logger) {}

std::vector<Model> LocalModelCatalog::FetchModels() const {
  std::lock_guard<std::mutex> guard(registration_mutex_);
  FileLock file_lock(lock_path_);
  const auto registrations = LoadRegistrations();

  std::vector<Model> models;
  models.reserve(registrations.size());
  for (const auto& registration : registrations) {
    models.push_back(CreateModel(registration));
  }

  return models;
}

Model* LocalModelCatalog::RegisterModel(const std::string& model_path_value, const std::string& model_id,
                                        const ModelInfo& metadata) {
  std::lock_guard<std::mutex> mutation_guard(kLocalCatalogMutationMutex);

  if (model_path_value.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model_path is required");
  }

  const auto parsed_id = ParseModelId(model_id);

  const std::filesystem::path supplied_path(model_path_value);
  if (HasParentTraversal(supplied_path)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model_path must not contain '..' path components");
  }

  std::error_code ec;
  if (!std::filesystem::is_directory(supplied_path, ec)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model_path must be an existing directory");
  }

  const auto model_path = std::filesystem::absolute(supplied_path).lexically_normal();
  const auto config_path = model_path / "genai_config.json";
  if (!std::filesystem::is_regular_file(config_path, ec)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model_path must contain a regular genai_config.json file");
  }

  GenAIConfig::LoadFromFile(config_path.string());

  const auto* task = metadata.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR);
  if (!task || task->empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "task is required");
  }
  if (!kSupportedTasks.contains(*task)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported task: " + *task);
  }

  ListModels();
  Registration registration;
  {
    std::lock_guard<std::mutex> guard(registration_mutex_);
    FileLock file_lock(lock_path_);
    auto registrations = LoadRegistrations();
    const auto duplicate = std::find_if(registrations.begin(), registrations.end(), [&](const auto& existing) {
      return existing.info.model_id == model_id;
    });
    if (duplicate != registrations.end()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model_id is already registered: " + model_id);
    }

    registration = {ResolveMetadata(metadata, model_id, parsed_id.name, parsed_id.version), model_path.string(),
                    std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count())};
    while (std::any_of(registrations.begin(), registrations.end(), [&](const auto& existing) {
      return existing.registration_id == registration.registration_id;
    })) {
      registration.registration_id += "-1";
    }

    registrations.push_back(registration);
    SaveRegistrations(registrations);
  }

  ListModels();
  auto* model = GetModelVariant(registration.info.model_id);
  const auto runtime_id = "local/" + registration.registration_id;
  if (!model || model->RuntimeId() != runtime_id) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "registered model was not available after catalog refresh");
  }

  return model;
}

void LocalModelCatalog::UnregisterModel(const std::string& alias_or_model_id) {
  std::lock_guard<std::mutex> mutation_guard(kLocalCatalogMutationMutex);

  if (alias_or_model_id.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "alias_or_model_id must not be empty");
  }

  ListModels();
  auto* model = GetModel(alias_or_model_id);
  if (!model) {
    auto* variant = GetModelVariant(alias_or_model_id);
    if (variant) {
      model = GetModel(variant->Alias());
    }
  }
  if (!model) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model not found: " + alias_or_model_id);
  }

  model->BeginUnregister();
  bool unregister_in_progress = true;
  try {
    for (const auto* variant : model->Variants()) {
      if (variant->IsLoaded()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot unregister a loaded model; unload it first");
      }
    }

    const bool unregister_alias = model->Alias() == alias_or_model_id;
    if (!unregister_alias && !model->PrepareRetireVariant(alias_or_model_id)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model not found: " + alias_or_model_id);
    }

    {
      std::lock_guard<std::mutex> guard(registration_mutex_);
      FileLock file_lock(lock_path_);
      auto registrations = LoadRegistrations();
      const auto end = std::remove_if(registrations.begin(), registrations.end(), [&](const auto& registration) {
        return registration.info.alias == alias_or_model_id || registration.info.model_id == alias_or_model_id;
      });
      if (end == registrations.end()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model not found: " + alias_or_model_id);
      }

      registrations.erase(end, registrations.end());
      SaveRegistrations(registrations);
    }

    if (!CommitUnregister(model, alias_or_model_id)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "unregistered model was not present in the in-memory catalog");
    }

    model->CancelUnregister();
    unregister_in_progress = false;
    ListModels();
  } catch (...) {
    if (unregister_in_progress) {
      model->CancelUnregister();
    }
    throw;
  }
}

ModelInfo LocalModelCatalog::ResolveMetadata(const ModelInfo& metadata, const std::string& model_id,
                                             const std::string& name, int version) const {
  auto resolved = metadata;
  RemoveLegacyRegistrationProperties(resolved);

  resolved.alias = name;
  resolved.name = name;
  resolved.version = version;
  resolved.model_id = model_id;
  resolved.uri.clear();
  if (!resolved.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR)) {
    SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR, "local");
  }
  SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR, "LocalRegistration");
  SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_ENTITY_TYPE_STR, "Model");
  SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR, "ONNX");

  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  SetModelInfoIntProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT, now);
  SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_CREATION_TIME_STR, FormatUtcTimestamp(now));

  return resolved;
}

std::vector<LocalModelCatalog::Registration> LocalModelCatalog::LoadRegistrations() const {
  std::ifstream stream(index_path_, std::ios::binary);
  if (!stream) {
    return {};
  }

  std::vector<Registration> registrations;
  try {
    nlohmann::json root;
    stream >> root;
    const auto schema_version = root.value("version", 0);
    if (!root.is_object() || (schema_version != 1 && schema_version != 2) || !root.contains("models") ||
      !root["models"].is_array()) {
      logger_.Log(LogLevel::Warning, "Ignoring malformed local model registration index: " + index_path_.string());
      return {};
    }

    for (const auto& item : root["models"]) {
      try {
        if (!item.is_object() || !item.contains("model_id") || !item["model_id"].is_string() ||
            !item.contains("model_path") || !item["model_path"].is_string() || !item.contains("properties")) {
          continue;
        }

        auto info = ModelInfoFromPropertyBagJson(item["properties"]);
        std::string registration_id;
        if (item.contains("registration_id") && item["registration_id"].is_string()) {
          registration_id = item["registration_id"].get<std::string>();
        } else if (const auto* legacy_registration_id = info.GetPropertyStr(kRegistrationIdProperty)) {
          registration_id = *legacy_registration_id;
        }
        if (registration_id.empty()) {
          continue;
        }

        const auto model_id = item["model_id"].get<std::string>();
        const auto parsed_id = ParseModelId(model_id);
        std::filesystem::path model_path = item["model_path"].get<std::string>();
        if (model_path.empty() || HasParentTraversal(model_path)) {
          continue;
        }
        model_path = std::filesystem::absolute(model_path).lexically_normal();

        RemoveLegacyRegistrationProperties(info);
        info.alias = parsed_id.name;
        info.name = parsed_id.name;
        info.version = parsed_id.version;
        info.model_id = model_id;
        info.uri.clear();

        const auto duplicate = std::find_if(registrations.begin(), registrations.end(), [&](const auto& existing) {
          return existing.info.model_id == info.model_id || existing.registration_id == registration_id;
        });
        if (duplicate == registrations.end()) {
          registrations.push_back({std::move(info), model_path.string(), std::move(registration_id)});
        }
      } catch (const std::exception& ex) {
        logger_.Log(LogLevel::Warning, std::string("Ignoring malformed local model registration: ") + ex.what());
      }
    }
  } catch (const std::exception& ex) {
    logger_.Log(LogLevel::Warning, std::string("Ignoring unreadable local model registration index: ") + ex.what());
  }

  return registrations;
}

void LocalModelCatalog::SaveRegistrations(const std::vector<Registration>& registrations) const {
  std::filesystem::create_directories(catalog_dir_);
  nlohmann::json models = nlohmann::json::array();
  for (const auto& registration : registrations) {
    models.push_back(RegistrationToJson(registration));
  }

  const nlohmann::json root = {{"version", 2}, {"catalog_name", "local"}, {"models", std::move(models)}};
  const auto temp_path = index_path_.string() + ".tmp";
  {
    std::ofstream stream(temp_path, std::ios::binary | std::ios::trunc);
    if (!stream) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to write local model registration index");
    }

    stream << root.dump(2) << '\n';
    if (!stream) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to write local model registration index");
    }
  }

#ifdef _WIN32
  if (!MoveFileExW(std::filesystem::path(temp_path).wstring().c_str(), index_path_.wstring().c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(temp_path);
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to commit local model registration index");
  }
#else
  std::error_code ec;
  std::filesystem::rename(temp_path, index_path_, ec);
  if (ec) {
    std::filesystem::remove(temp_path);
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to commit local model registration index: " + ec.message());
  }
#endif
}

Model LocalModelCatalog::CreateModel(const Registration& registration) const {
  return model_factory_(registration.info, registration.model_path, "local/" + registration.registration_id);
}

}  // namespace fl
