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
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <system_error>
#include <regex>
#include <unordered_set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fl {
namespace {

constexpr const char* kLegacyRegistrationIdProperty = "_local_registration_id";
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

ParsedModelId ValidateRegistrationMetadata(const std::string& model_id, const ModelInfo& metadata) {
  auto parsed_id = ParseModelId(model_id);

  const auto* task = metadata.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR);
  if (!task || task->empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "task is required");
  }
  if (!kSupportedTasks.contains(*task)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unsupported task: " + *task);
  }

  return parsed_id;
}

std::string DeriveAlias(std::string_view name) {
  static constexpr std::array<std::string_view, 3> kGenericSuffixes = {
      "-generic-cpu",
      "-generic-gpu",
      "-generic-npu",
  };

  for (const auto suffix : kGenericSuffixes) {
    if (name.size() > suffix.size() && name.ends_with(suffix)) {
      return std::string(name.substr(0, name.size() - suffix.size()));
    }
  }

  return std::string(name);
}

void RemoveLegacyRegistrationProperties(ModelInfo& info) {
  info.string_properties.erase(kLegacyModelPathProperty);
  info.string_properties.erase(kLegacyAliasProperty);
  info.string_properties.erase(kLegacyRegistrationIdProperty);
  info.string_properties.erase(kLegacyVersionProperty);
  info.int_properties.erase(kLegacyModelPathProperty);
  info.int_properties.erase(kLegacyAliasProperty);
  info.int_properties.erase(kLegacyRegistrationIdProperty);
  info.int_properties.erase(kLegacyVersionProperty);
}

bool IsLegacyIntegerProperty(std::string_view key) {
  return key == FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_TOOL_CALLING_INT ||
         key == FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_REASONING_INT ||
         key == FOUNDRY_LOCAL_MODEL_PROP_SUPPORTS_HYBRID_REASONING_INT ||
         key == FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT ||
         key == FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT ||
         key == FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT ||
         key == FOUNDRY_LOCAL_MODEL_PROP_IS_TEST_MODEL_INT ||
         key == FOUNDRY_LOCAL_MODEL_PROP_CONTEXT_LENGTH_INT;
}

ModelInfo ModelInfoFromLegacyPropertyBagJson(const nlohmann::json& json) {
  if (!json.is_object()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "legacy model info properties must contain an object");
  }

  ModelInfo info;
  for (const auto& [key, value] : json.items()) {
    if (value.is_string()) {
      auto text = value.get<std::string>();
      if (IsLegacyIntegerProperty(key)) {
        int64_t integer = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), integer);
        if (error != std::errc{} || end != text.data() + text.size()) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid legacy integer model info property: " + key);
        }

        info.SetPropertyInt(key, integer);
      } else {
        info.SetPropertyStr(key, std::move(text));
      }
    } else if (value.is_number_integer()) {
      info.SetPropertyInt(key, value.get<int64_t>());
    } else {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               "legacy model info property values must be strings or integers");
    }
  }

  return info;
}

nlohmann::json RegistrationToJson(const LocalModelCatalog::Registration& registration) {
  return {
      {"model_info", ModelInfoToJson(registration.info)},
      {"model_path", registration.model_path},
  };
}

}  // namespace

LocalModelCatalog::LocalModelCatalog(std::filesystem::path model_cache_dir, ModelFactory model_factory,
                                     ILogger& logger)
    : BaseModelCatalog("local", CatalogType::kLocal, logger),
      model_cache_dir_(std::move(model_cache_dir)),
      index_path_(model_cache_dir_ / "foundry.local.modelinfo.json"),
      lock_path_(model_cache_dir_ / "foundry.local.modelinfo.lock"),
      model_factory_(std::move(model_factory)) {}

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

  const auto parsed_id = ValidateRegistrationMetadata(model_id, metadata);

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

    registration = {ResolveMetadata(metadata, model_id, parsed_id.name, parsed_id.version), model_path.string()};

    registrations.push_back(registration);
    SaveRegistrations(registrations);
  }

  ListModels();
  auto* model = GetModelVariant(registration.info.model_id);
  if (!model || model->Id() != registration.info.model_id) {
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
  auto* target_variant = static_cast<Model*>(nullptr);
  if (!model) {
    target_variant = GetModelVariant(alias_or_model_id);
    if (target_variant) {
      model = GetModel(target_variant->Alias());
    }
  }
  if (!model) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model not found: " + alias_or_model_id);
  }

  model->BeginUnregister();
  bool unregister_in_progress = true;
  try {
    const bool unregister_alias = model->Alias() == alias_or_model_id;
    if (unregister_alias) {
      for (const auto* variant : model->Variants()) {
        if (variant->IsLoaded()) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot unregister a loaded model; unload it first");
        }
      }
    } else if (target_variant->IsLoaded()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot unregister a loaded model; unload it first");
    }

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

    model->EndUnregister();
    unregister_in_progress = false;
    ListModels();
  } catch (...) {
    if (unregister_in_progress) {
      model->EndUnregister();
    }
    throw;
  }
}

ModelInfo LocalModelCatalog::ResolveMetadata(const ModelInfo& metadata, const std::string& model_id,
                                             const std::string& name, int version) const {
  auto resolved = metadata;
  RemoveLegacyRegistrationProperties(resolved);

  resolved.alias = DeriveAlias(name);
  resolved.name = name;
  resolved.version = version;
  resolved.model_id = model_id;
  resolved.uri.clear();
  if (!resolved.GetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR)) {
    resolved.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR, "local");
  }
  resolved.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR, "LocalRegistration");
  resolved.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_ENTITY_TYPE_STR, "Model");
  resolved.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR, "ONNX");

  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  resolved.SetPropertyInt(FOUNDRY_LOCAL_MODEL_PROP_CREATED_AT_UNIX_INT, now);
  resolved.SetPropertyStr(FOUNDRY_LOCAL_MODEL_PROP_CREATION_TIME_STR, FormatUtcTimestamp(now));

  return resolved;
}

std::vector<LocalModelCatalog::Registration> LocalModelCatalog::LoadRegistrations() const {
  std::error_code exists_error;
  const bool index_exists = std::filesystem::exists(index_path_, exists_error);
  if (exists_error) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "failed to inspect local model registration index: " + exists_error.message());
  }
  if (!index_exists) {
    return {};
  }

  std::ifstream stream(index_path_, std::ios::binary);
  if (!stream) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to open local model registration index: " + index_path_.string());
  }

  nlohmann::json root;
  try {
    stream >> root;
  } catch (const std::exception& ex) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to parse local model registration index: " + std::string(ex.what()));
  }

  if (!root.is_object()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "unsupported or malformed local model registration index: " + index_path_.string());
  }

  const auto schema_version = root.value("version", 0);
  if ((schema_version != 1 && schema_version != 2) || !root.contains("models") || !root["models"].is_array()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "unsupported or malformed local model registration index: " + index_path_.string());
  }

  std::vector<Registration> registrations;
  size_t item_index = 0;
  for (const auto& item : root["models"]) {
    try {
      if (!item.is_object() || !item.contains("model_path") || !item["model_path"].is_string()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model_path must be a string");
      }

      ModelInfo info;
      std::string model_id;
      if (schema_version == 2) {
        if (!item.contains("model_info") || !item["model_info"].is_object()) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model_info must be an object");
        }

        info = ModelInfoFromJson(item["model_info"]);
        model_id = info.model_id;
      } else {
        if (!item.contains("model_id") || !item["model_id"].is_string() || !item.contains("properties")) {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "legacy registration is missing model metadata");
        }

        info = ModelInfoFromLegacyPropertyBagJson(item["properties"]);
        model_id = item["model_id"].get<std::string>();
      }

      const auto parsed_id = ValidateRegistrationMetadata(model_id, info);
      std::filesystem::path model_path = item["model_path"].get<std::string>();
      if (model_path.empty() || HasParentTraversal(model_path)) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "model_path is empty or contains '..' components");
      }
      model_path = std::filesystem::absolute(model_path).lexically_normal();

      RemoveLegacyRegistrationProperties(info);
      info.alias = DeriveAlias(parsed_id.name);
      info.name = parsed_id.name;
      info.version = parsed_id.version;
      info.model_id = model_id;
      info.uri.clear();

      const auto duplicate = std::find_if(registrations.begin(), registrations.end(), [&](const auto& existing) {
        return existing.info.model_id == info.model_id;
      });
      if (duplicate != registrations.end()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "duplicate model_id: " + info.model_id);
      }

      registrations.push_back({std::move(info), model_path.string()});
    } catch (const std::exception& ex) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "invalid local model registration at index " + std::to_string(item_index) + ": " + ex.what());
    }

    ++item_index;
  }

  return registrations;
}

void LocalModelCatalog::SaveRegistrations(const std::vector<Registration>& registrations) const {
  std::filesystem::create_directories(model_cache_dir_);
  nlohmann::json models = nlohmann::json::array();
  for (const auto& registration : registrations) {
    models.push_back(RegistrationToJson(registration));
  }

  const nlohmann::json root = {{"version", 2}, {"catalog_name", "local"}, {"models", std::move(models)}};
  const auto serialized = root.dump(2) + '\n';
  const auto temp_path = index_path_.string() + ".tmp";
  const auto remove_temp = [&temp_path]() noexcept {
    std::error_code ec;
    std::filesystem::remove(temp_path, ec);
  };

  std::ofstream stream(temp_path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    remove_temp();
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to write local model registration index");
  }

  stream << serialized;
  stream.flush();
  if (!stream) {
    stream.close();
    remove_temp();
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to write local model registration index");
  }

  stream.close();
  if (stream.fail()) {
    remove_temp();
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to write local model registration index");
  }

#ifdef _WIN32
  if (!MoveFileExW(std::filesystem::path(temp_path).wstring().c_str(), index_path_.wstring().c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    remove_temp();
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to commit local model registration index");
  }
#else
  std::error_code ec;
  std::filesystem::rename(temp_path, index_path_, ec);
  if (ec) {
    remove_temp();
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to commit local model registration index: " + ec.message());
  }
#endif
}

Model LocalModelCatalog::CreateModel(const Registration& registration) const {
  return model_factory_(registration.info, registration.model_path);
}

}  // namespace fl
