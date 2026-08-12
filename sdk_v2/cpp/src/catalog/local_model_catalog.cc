// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/local_model_catalog.h"

#include "exception.h"
#include "inferencing/generative/genai_config.h"
#include "util/file_lock.h"

#include <foundry_local/foundry_local_c.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fl {
namespace {

constexpr const char* kRegistrationIdProperty = "_local_registration_id";

std::string UtcTimestamp(int64_t unix_time) {
  std::time_t value = static_cast<std::time_t>(unix_time);
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
  for (const auto& component : path) {
    if (component == "..") {
      return true;
    }
  }
  return false;
}

int64_t DirectorySize(const std::filesystem::path& path) {
  // Best-effort deterministic metadata decoration only. This does not discover registrations or validate assets;
  // catalog membership comes exclusively from the flat per-catalog registration index.
  std::error_code ec;
  if (!std::filesystem::is_directory(path, ec)) {
    return 0;
  }

  int64_t total = 0;
  for (std::filesystem::recursive_directory_iterator it(path, std::filesystem::directory_options::skip_permission_denied,
                                                         ec), end;
       it != end; it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    if (!it->is_regular_file(ec) || it->path().filename() == "model_metadata.yml") {
      continue;
    }
    total += static_cast<int64_t>(it->file_size(ec));
    ec.clear();
  }
  return total;
}

std::string EscapeYaml(std::string_view value) {
  std::string result{"\""};
  for (const char ch : value) {
    if (ch == '\\' || ch == '"') {
      result.push_back('\\');
    }
    if (ch == '\n') {
      result += "\\n";
    } else if (ch != '\r') {
      result.push_back(ch);
    }
  }
  result.push_back('"');
  return result;
}

void WriteOptionalYamlString(std::ostream& stream, const ModelInfo& info, const char* key, const char* yaml_key) {
  const auto* value = info.GetPropertyStr(key);
  if (value && !value->empty()) {
    stream << yaml_key << ": " << EscapeYaml(*value) << '\n';
  }
}

void WriteOptionalYamlInt(std::ostream& stream, const ModelInfo& info, const char* key, const char* yaml_key) {
  const auto* value = info.GetPropertyInt(key);
  if (value) {
    stream << yaml_key << ": " << *value << '\n';
  }
}

nlohmann::json RegistrationToJson(const LocalModelCatalog::Registration& registration) {
  return {
      {"alias", registration.info.alias},
      {"model_path", registration.model_path},
      {"registered_at", registration.info.GetPropertyWithDefault(FOUNDRY_LOCAL_MODEL_PROP_CREATION_TIME_STR, {})},
      {"properties", ModelInfoToPropertyBagJson(registration.info)},
      {"metadata_prepared", registration.metadata_prepared},
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
  FileLock file_lock(lock_path_);
  std::vector<Model> models;
  for (const auto& registration : LoadRegistrations()) {
    models.push_back(CreateModel(registration));
  }
  return models;
}

Model* LocalModelCatalog::RegisterModel(const ModelInfo& model_info) {
  const auto* model_path_value = model_info.GetPropertyStr(FOUNDRY_LOCAL_REG_MODEL_PATH);
  const auto* alias_value = model_info.GetPropertyStr(FOUNDRY_LOCAL_REG_ALIAS);
  if (!model_path_value || model_path_value->empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model_path is required");
  }
  if (!alias_value || alias_value->empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "alias is required");
  }
  if (!std::regex_match(*alias_value, std::regex("[A-Za-z0-9][A-Za-z0-9._-]*"))) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "alias must match [a-zA-Z0-9][a-zA-Z0-9._-]*");
  }

  std::filesystem::path supplied_path(*model_path_value);
  if (HasParentTraversal(supplied_path)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model_path must not contain '..' path components");
  }

  const auto model_path = std::filesystem::absolute(supplied_path).lexically_normal().string();
  ListModels();
  Registration registration;
  {
    std::lock_guard<std::mutex> guard(registration_mutex_);
    FileLock file_lock(lock_path_);
    auto registrations = LoadRegistrations();
    for (const auto& existing : registrations) {
      if (existing.info.alias == *alias_value) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                 "a model with alias '" + *alias_value + "' is already registered");
      }
    }

    bool assets_inspected = false;
    registration = {ResolveMetadata(model_info, nullptr, model_path, *alias_value, &assets_inspected), model_path,
            assets_inspected};
    auto registration_id = registration.info.GetPropertyWithDefault(kRegistrationIdProperty, std::string{});
    while (std::any_of(registrations.begin(), registrations.end(), [&](const Registration& existing) {
      return existing.info.GetPropertyWithDefault(kRegistrationIdProperty, std::string{}) == registration_id;
    })) {
      registration_id += "-1";
    }
    SetModelInfoStringProperty(registration.info, kRegistrationIdProperty, std::move(registration_id));
    WriteMetadata(registration);
    registrations.push_back(registration);
    SaveRegistrations(registrations);
  }

  try {
    return AppendActiveModel(CreateModel(registration));
  } catch (...) {
    std::lock_guard<std::mutex> guard(registration_mutex_);
    FileLock file_lock(lock_path_);
    auto registrations = LoadRegistrations();
    registrations.erase(std::remove_if(registrations.begin(), registrations.end(), [&](const Registration& entry) {
                          return entry.info.GetPropertyWithDefault(kRegistrationIdProperty, std::string{}) ==
                                 registration.info.GetPropertyWithDefault(kRegistrationIdProperty, std::string{});
                        }),
                        registrations.end());
    SaveRegistrations(registrations);
    throw;
  }
}

void LocalModelCatalog::UnregisterModel(const std::string& alias_or_model_id) {
  if (alias_or_model_id.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "alias_or_model_id must not be empty");
  }

  auto* model = GetModel(alias_or_model_id);
  if (!model) {
    model = GetModelVariant(alias_or_model_id);
  }
  if (!model) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model not found: " + alias_or_model_id);
  }
  model->BeginUnregister();
  bool unregister_lock_held = true;
  try {
    if (model->IsLoaded()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot unregister a loaded model; unload it first");
    }

    {
      std::lock_guard<std::mutex> guard(registration_mutex_);
      FileLock file_lock(lock_path_);
      auto registrations = LoadRegistrations();
      auto end = std::remove_if(registrations.begin(), registrations.end(), [&](const Registration& registration) {
        return registration.info.alias == alias_or_model_id || registration.info.model_id == alias_or_model_id;
      });
      if (end == registrations.end()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model not found: " + alias_or_model_id);
      }

      registrations.erase(end, registrations.end());
      SaveRegistrations(registrations);
    }

    RetireModel(alias_or_model_id);
    model->CancelUnregister();
    unregister_lock_held = false;
  } catch (...) {
    if (unregister_lock_held) {
      model->CancelUnregister();
    }
    throw;
  }
}

std::vector<Model*> LocalModelCatalog::GetLocalModels() const {
  return ListModels();
}

ModelInfo LocalModelCatalog::ResolveMetadata(const ModelInfo& metadata, const ModelInfo* previous,
                                             const std::string& model_path, const std::string& alias,
                                             bool* assets_inspected) const {
  if (assets_inspected) {
    *assets_inspected = false;
  }
  auto resolved = previous ? *previous : metadata;
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
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
  if (!previous) {
    SetModelInfoIntProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT, now);
    SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_CREATION_TIME_STR, UtcTimestamp(now));
  }
  if (!previous) {
    const auto registration_id = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    SetModelInfoStringProperty(resolved, kRegistrationIdProperty, std::to_string(registration_id));
  }
  const auto config_path = std::filesystem::path(model_path) / "genai_config.json";
  try {
    if (std::filesystem::exists(config_path)) {
      const auto config = GenAIConfig::LoadFromFile(config_path.string());
      if (assets_inspected) {
        *assets_inspected = true;
      }
      SetModelInfoIntProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_BYTES_INT, DirectorySize(model_path));
      resolved.int_properties.erase(FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT);
      if (config.model && config.model->context_length > 0) {
        SetModelInfoIntProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, config.model->context_length);
      }
      // Keep the default provider in genai_config.json authoritative when the caller did not supply one. Some OGA
      // providers such as DML are not represented by the SDK's explicit ExecutionProvider enum and use kDefault.
      std::string task = "chat-completion";
      if (config.hidden_size) {
        task = "embeddings";
      } else if (config.model && config.model->type == "whisper") {
        task = "automatic-speech-recognition";
      }
      SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, task);
      SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR,
                                 task == "automatic-speech-recognition" ? "audio" : "language");
      SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR, "language");
    }
  } catch (const std::exception& ex) {
    logger_.Log(LogLevel::Warning, "Ignoring BYOM metadata inspection failure for '" + model_path + "': " + ex.what());
  }

  if (resolved.task.empty()) {
    SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "chat-completion");
  }
  if (!resolved.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR)) {
    SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR,
                               resolved.task == "automatic-speech-recognition" ? "audio" : "language");
  }
  if (!resolved.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR)) {
    SetModelInfoStringProperty(resolved, FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR, "language");
  }
  return resolved;
}

std::vector<LocalModelCatalog::Registration> LocalModelCatalog::LoadRegistrations() const {
  std::vector<Registration> registrations;
  std::ifstream stream(index_path_, std::ios::binary);
  if (!stream) {
    return registrations;
  }

  try {
    nlohmann::json root;
    stream >> root;
    if (!root.is_object() || root.value("version", 0) != 1 || !root.contains("models") || !root["models"].is_array()) {
      logger_.Log(LogLevel::Warning, "Ignoring malformed local model registration index: " + index_path_.string());
      return registrations;
    }

    for (const auto& item : root["models"]) {
      try {
        if (!item.is_object() || !item.contains("model_path") || !item["model_path"].is_string() ||
            !item.contains("properties")) {
          continue;
        }
        if (item.contains("metadata_prepared") && !item["metadata_prepared"].is_boolean()) {
          logger_.Log(LogLevel::Warning, "Ignoring local model registration with invalid metadata preparation state");
          continue;
        }

        auto info = ModelInfoFromPropertyBagJson(item["properties"]);
        const auto* registration_id = info.GetPropertyStr(kRegistrationIdProperty);
        if (!registration_id || registration_id->empty()) {
          logger_.Log(LogLevel::Warning, "Ignoring local model registration missing its stable registration ID");
          continue;
        }
        const auto* alias = info.GetPropertyStr(FOUNDRY_LOCAL_REG_ALIAS);
        if (!alias || !std::regex_match(*alias, std::regex("[A-Za-z0-9][A-Za-z0-9._-]*"))) {
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
        SetModelInfoStringProperty(info, FOUNDRY_LOCAL_REG_MODEL_PATH, model_path.string());
        const auto duplicate = std::find_if(registrations.begin(), registrations.end(), [&](const Registration& entry) {
          return entry.info.alias == info.alias || entry.info.model_id == info.model_id ||
                 entry.info.GetPropertyWithDefault(kRegistrationIdProperty, std::string{}) == *registration_id;
        });
        if (duplicate == registrations.end()) {
          registrations.push_back(
              {std::move(info), model_path.string(), item.value("metadata_prepared", false)});
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

std::optional<ModelInfo> LocalModelCatalog::PrepareRegistrationMetadata(const std::string& registration_id) const {
  std::lock_guard<std::mutex> guard(registration_mutex_);
  FileLock file_lock(lock_path_);
  auto registrations = LoadRegistrations();
  auto it = std::find_if(registrations.begin(), registrations.end(), [&](const Registration& registration) {
    return registration.info.GetPropertyWithDefault(kRegistrationIdProperty, std::string{}) == registration_id;
  });
  if (it == registrations.end()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model is no longer registered");
  }
  if (it->metadata_prepared) {
    const auto metadata_path = std::filesystem::path(it->model_path) / "model_metadata.yml";
    if (!std::filesystem::is_regular_file(metadata_path)) {
      WriteMetadata(*it);
    }
    return it->info;
  }

  bool assets_inspected = false;
  auto refreshed = ResolveMetadata(it->info, &it->info, it->model_path, it->info.alias, &assets_inspected);
  if (!assets_inspected) {
    return std::nullopt;
  }

  it->info = std::move(refreshed);
  it->metadata_prepared = true;
  WriteMetadata(*it);
  SaveRegistrations(registrations);
  return it->info;
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

void LocalModelCatalog::WriteMetadata(const Registration& registration) const {
  // This portable model-side metadata artifact is distinct from registration persistence. The flat catalog index is
  // authoritative for membership, and unregistering never mutates user-owned model files.
  const auto path = std::filesystem::path(registration.model_path);
  std::error_code ec;
  if (!std::filesystem::is_directory(path, ec)) {
    return;
  }

  const auto metadata_path = path / "model_metadata.yml";
  const auto temp_path = path / "model_metadata.yml.tmp";
  {
    std::ofstream stream(temp_path, std::ios::binary | std::ios::trunc);
    if (!stream) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "failed to write model_metadata.yml beside BYOM assets: " + registration.model_path);
    }

    const auto& info = registration.info;
    stream << "schema_version: 1\n";
    stream << "name: " << EscapeYaml(info.name) << '\n';
    stream << "version: " << info.version << '\n';
    WriteOptionalYamlString(stream, info, FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR, "publisher");
    stream << "alias: " << EscapeYaml(info.alias) << '\n';
    WriteOptionalYamlString(stream, info, FOUNDRY_LOCAL_MODEL_PROP_DISPLAY_NAME_STR, "display_name");
    stream << "foundry_local: true\n";
    stream << "type: \"Model\"\n";
    stream << "model_type: \"ONNX\"\n";
    WriteOptionalYamlInt(stream, info, FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_BYTES_INT, "file_size_bytes");
    WriteOptionalYamlString(stream, info, FOUNDRY_LOCAL_MODEL_PROP_CREATION_TIME_STR, "creation_time");
    WriteOptionalYamlInt(stream, info, FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT, "context_length");
    WriteOptionalYamlString(stream, info, FOUNDRY_LOCAL_MODEL_PROP_EP_STR, "execution_provider");
    WriteOptionalYamlString(stream, info, FOUNDRY_LOCAL_MODEL_PROP_DEVICE_TYPE_STR, "device");
    WriteOptionalYamlString(stream, info, FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "task");
    WriteOptionalYamlString(stream, info, FOUNDRY_LOCAL_MODEL_PROP_LICENSE_STR, "license");
    WriteOptionalYamlString(stream, info, FOUNDRY_LOCAL_MODEL_PROP_LICENSE_DESCRIPTION_STR, "license_description");
    WriteOptionalYamlInt(stream, info, FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT, "max_output_tokens");
    WriteOptionalYamlString(stream, info, FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR, "input_modalities");
    WriteOptionalYamlString(stream, info, FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR, "output_modalities");
    WriteOptionalYamlString(stream, info, FOUNDRY_LOCAL_MODEL_PROP_MIN_FL_VERSION_STR, "min_foundry_local_version");
    WriteOptionalYamlString(stream, info, FOUNDRY_LOCAL_MODEL_PROP_AUTHOR_STR, "author");
    WriteOptionalYamlString(stream, info, FOUNDRY_LOCAL_MODEL_PROP_QUANTIZATION_STR, "quantization");
    WriteOptionalYamlString(stream, info, FOUNDRY_LOCAL_MODEL_PROP_CAPABILITIES_STR, "capabilities");
    const auto write_bool = [&](const char* property_key, const char* yaml_key) {
      const auto* value = info.GetPropertyInt(property_key);
      if (value) {
        stream << yaml_key << ": " << (*value != 0 ? "true" : "false") << '\n';
      }
    };
    write_bool(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT, "supports_tool_calling");
    write_bool(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT, "supports_reasoning");
    write_bool(FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_HYBRID_REASONING_INT, "supports_hybrid_reasoning");
    if (!stream) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "failed to write model_metadata.yml beside BYOM assets: " + registration.model_path);
    }
  }

#ifdef _WIN32
  if (!MoveFileExW(temp_path.wstring().c_str(), metadata_path.wstring().c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(temp_path);
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to commit model_metadata.yml: " + registration.model_path);
  }
#else
  std::filesystem::rename(temp_path, metadata_path, ec);
  if (ec) {
    std::filesystem::remove(temp_path);
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to commit model_metadata.yml: " + ec.message());
  }
#endif
}

Model LocalModelCatalog::CreateModel(const Registration& registration) const {
  const auto registration_id = registration.info.GetPropertyWithDefault(kRegistrationIdProperty, std::string{});
  return model_factory_(
      registration.info, registration.model_path,
      [this, registration_id](const std::string& model_id) {
        auto* current = GetModelVariant(model_id);
        if (!current || !current->IsActive() ||
            current->Info().GetPropertyWithDefault(kRegistrationIdProperty, std::string{}) != registration_id) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "model is no longer registered");
        }
        const_cast<LocalModelCatalog*>(this)->UnregisterModel(model_id);
      },
      [this, registration_id]() { return PrepareRegistrationMetadata(registration_id); });
}

}  // namespace fl
