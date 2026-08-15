// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/local_model_catalog.h"

#include "exception.h"
#include "inferencing/generative/genai_config.h"
#include "util/file_lock.h"

#include <foundry_local/foundry_local_c.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fl {
namespace {

constexpr const char* kRegistrationIdProperty = "_local_registration_id";
const std::regex kAliasPattern("[A-Za-z0-9][A-Za-z0-9._-]*");
const std::unordered_set<std::string> kSupportedTasks = {
    "automatic-speech-recognition",
    "chat-completion",
    "embeddings",
    "vision-language-chat",
};

std::string UtcTimestamp(int64_t unix_time) {
  const auto value = static_cast<std::time_t>(unix_time);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &value);
#else
  gmtime_r(&value, &utc);
#endif

  std::ostringstream stream;
  stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return stream.str();
}

bool HasParentTraversal(const std::filesystem::path& path) {
  return std::any_of(path.begin(), path.end(), [](const auto& component) { return component == ".."; });
}

nlohmann::json RegistrationToJson(const LocalModelCatalog::Registration& registration) {
  return {
      {"alias", registration.info.alias},
      {"model_id", registration.info.model_id},
      {"model_path", registration.model_path},
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

Model* LocalModelCatalog::RegisterModel(const ModelInfo& model_info) {
  const auto* model_path_value = model_info.GetPropertyStr(FOUNDRY_LOCAL_REG_MODEL_PATH);
  if (!model_path_value || model_path_value->empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model_path is required");
  }

  const auto* alias_value = model_info.GetPropertyStr(FOUNDRY_LOCAL_REG_ALIAS);
  if (!alias_value || alias_value->empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "alias is required");
  }

  if (!std::regex_match(*alias_value, kAliasPattern)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "alias must match [a-zA-Z0-9][a-zA-Z0-9._-]*");
  }

  const std::filesystem::path supplied_path(*model_path_value);
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

  const auto* task = model_info.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR);
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
      return existing.info.alias == *alias_value;
    });
    if (duplicate != registrations.end()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "a model with alias '" + *alias_value + "' is already registered");
    }

    registration = {ResolveMetadata(model_info, model_path.string(), *alias_value), model_path.string()};
    auto registration_id = registration.info.GetPropertyWithDefault(kRegistrationIdProperty, std::string{});
    while (std::any_of(registrations.begin(), registrations.end(), [&](const auto& existing) {
      return existing.info.GetPropertyWithDefault(kRegistrationIdProperty, std::string{}) == registration_id;
    })) {
      registration_id += "-1";
    }
    SetModelInfoStringProperty(registration.info, kRegistrationIdProperty, std::move(registration_id));

    registrations.push_back(registration);
    SaveRegistrations(registrations);
  }

  ListModels();
  auto* model = GetModelVariant(registration.info.model_id);
  const auto runtime_id =
      "local/" + registration.info.GetPropertyWithDefault(kRegistrationIdProperty, std::string{});
  if (!model || model->RuntimeId() != runtime_id) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "registered model was not available after catalog refresh");
  }

  return model;
}

void LocalModelCatalog::UnregisterModel(const std::string& alias_or_model_id) {
  if (alias_or_model_id.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "alias_or_model_id must not be empty");
  }

  ListModels();
  auto* model = GetModel(alias_or_model_id);
  if (!model) {
    model = GetModelVariant(alias_or_model_id);
  }
  if (!model) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model not found: " + alias_or_model_id);
  }

  model->BeginUnregister();
  try {
    if (model->IsLoaded()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot unregister a loaded model; unload it first");
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

    ListModels();
    model->CancelUnregister();
  } catch (...) {
    model->CancelUnregister();
    throw;
  }
}

ModelInfo LocalModelCatalog::ResolveMetadata(const ModelInfo& metadata, const std::string& model_path,
                                             const std::string& alias) const {
  auto resolved = metadata;
  const auto version = resolved.GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_VERSION_INT, int64_t{0});
  if (version < 0 || version > std::numeric_limits<int>::max()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "version must be a non-negative integer");
  }

  resolved.alias = alias;
  resolved.name = alias;
  resolved.version = static_cast<int>(version);
  resolved.model_id = alias + ":" + std::to_string(version);
  resolved.uri.clear();
  SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_REG_MODEL_PATH, model_path);
  SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_REG_ALIAS, alias);
  SetModelInfoIntProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_VERSION_INT, version);
  if (!resolved.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR)) {
    SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR, "local");
  }
  SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR, "LocalRegistration");
  SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_ENTITY_TYPE_STR, "Model");
  SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR, "ONNX");

  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  SetModelInfoIntProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT, now);
  SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_CREATION_TIME_STR, UtcTimestamp(now));
  const auto registration_id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  SetModelInfoStringProperty(resolved, kRegistrationIdProperty, std::to_string(registration_id));

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
    if (!root.is_object() || root.value("version", 0) != 1 || !root.contains("models") ||
        !root["models"].is_array()) {
      logger_.Log(LogLevel::Warning, "Ignoring malformed local model registration index: " + index_path_.string());
      return {};
    }

    for (const auto& item : root["models"]) {
      try {
        if (!item.is_object() || !item.contains("model_path") || !item["model_path"].is_string() ||
            !item.contains("properties")) {
          continue;
        }

        auto info = ModelInfoFromPropertyBagJson(item["properties"]);
        const auto* registration_id = info.GetPropertyStr(kRegistrationIdProperty);
        const auto* alias = info.GetPropertyStr(FOUNDRY_LOCAL_REG_ALIAS);
        if (!registration_id || registration_id->empty() || !alias || !std::regex_match(*alias, kAliasPattern)) {
          continue;
        }

        const auto version = info.GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_VERSION_INT, int64_t{0});
        if (version < 0 || version > std::numeric_limits<int>::max()) {
          continue;
        }

        std::filesystem::path model_path = item["model_path"].get<std::string>();
        if (model_path.empty() || HasParentTraversal(model_path)) {
          continue;
        }
        model_path = std::filesystem::absolute(model_path).lexically_normal();

        info.alias = *alias;
        info.name = *alias;
        info.version = static_cast<int>(version);
        info.model_id = info.alias + ":" + std::to_string(info.version);
        info.uri.clear();
        SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_MODEL_PATH, model_path.string());

        const auto duplicate = std::find_if(registrations.begin(), registrations.end(), [&](const auto& existing) {
          return existing.info.alias == info.alias || existing.info.model_id == info.model_id ||
                 existing.info.GetPropertyWithDefault(kRegistrationIdProperty, std::string{}) == *registration_id;
        });
        if (duplicate == registrations.end()) {
          registrations.push_back({std::move(info), model_path.string()});
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

  const nlohmann::json root = {{"version", 1}, {"catalog_name", "local"}, {"models", std::move(models)}};
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
  const auto registration_id = registration.info.GetPropertyWithDefault(kRegistrationIdProperty, std::string{});
  return model_factory_(registration.info, registration.model_path, "local/" + registration_id);
}

}  // namespace fl
