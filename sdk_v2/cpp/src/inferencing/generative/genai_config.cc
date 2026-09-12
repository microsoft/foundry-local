// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/generative/genai_config.h"
#include "exception.h"

#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

namespace fl {
namespace {

size_t ParsePositiveSize(const nlohmann::json& object,
                         const char* object_path,
                         const char* name,
                         size_t default_value) {
  if (!object.contains(name)) {
    return default_value;
  }

  const auto& value = object[name];
  if (!value.is_number_integer()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             std::string("genai_config.json ") + object_path + "." + name + " must be a positive integer");
  }

  uint64_t parsed;
  try {
    if (value.is_number_unsigned()) {
      parsed = value.get<uint64_t>();
    } else {
      const auto signed_value = value.get<int64_t>();
      if (signed_value <= 0) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
                 std::string("genai_config.json ") + object_path + "." + name + " must be a positive integer");
      }
      parsed = static_cast<uint64_t>(signed_value);
    }
  } catch (const nlohmann::json::exception&) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             std::string("genai_config.json ") + object_path + "." + name + " must be a positive integer");
  }

  if (parsed == 0 || parsed > std::numeric_limits<size_t>::max()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             std::string("genai_config.json ") + object_path + "." + name + " must be a positive integer");
  }

  return static_cast<size_t>(parsed);
}

}  // namespace

bool GenAIConfig::OnnxModel::IsMultiModal() const {
  return type == "phi3v" || type == "whisper" || type == "phi4mm" || type == "fara" ||
         type == "qwen2_5_vl" || type == "qwen3_vl" || type == "qwen3_5" || type == "gemma4" ||
         type == "mistral3";
}

std::string GenAIConfig::DefaultProvider() const {
  if (!model || !model->decoder || !model->decoder->session_options) {
    return "";
  }

  const auto& provider_options = model->decoder->session_options->provider_options;
  if (provider_options.empty()) {
    return "";
  }

  // The first key of the first provider_options entry is the default provider name.
  const auto& first = provider_options.front();
  if (first.empty()) {
    return "";
  }

  return first.begin()->first;
}

ChatBackendKind GenAIConfig::GetChatBackendKind() const {
  if (!engine) {
    return ChatBackendKind::kGenerator;
  }

  if (engine->dynamic_batching) {
    return ChatBackendKind::kEngine;
  }

  return ChatBackendKind::kGenerator;
}

std::optional<size_t> GenAIConfig::EngineMaxBatchSize() const {
  switch (GetChatBackendKind()) {
    case ChatBackendKind::kEngine:
      return engine->dynamic_batching->max_batch_size;
    case ChatBackendKind::kGenerator:
      return std::nullopt;
  }

  return std::nullopt;
}

GenAIConfig GenAIConfig::LoadFromFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "failed to open genai_config.json: " + path);
  }

  nlohmann::json j;
  try {
    file >> j;
  } catch (const nlohmann::json::parse_error& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, std::string("failed to parse genai_config.json: ") + e.what());
  }

  GenAIConfig config;

  if (j.contains("model") && j["model"].is_object()) {
    const auto& jm = j["model"];
    OnnxModel model;

    if (jm.contains("context_length") && jm["context_length"].is_number()) {
      model.context_length = jm["context_length"].get<int64_t>();
    }

    if (jm.contains("type") && jm["type"].is_string()) {
      model.type = jm["type"].get<std::string>();
    }

    if (jm.contains("prompt_templates") && jm["prompt_templates"].is_object()) {
      for (auto& [key, val] : jm["prompt_templates"].items()) {
        if (val.is_string()) {
          model.prompt_templates[key] = val.get<std::string>();
        }
      }
    }

    if (jm.contains("decoder") && jm["decoder"].is_object()) {
      const auto& jd = jm["decoder"];
      OnnxModel::Decoder decoder;

      if (jd.contains("session_options") && jd["session_options"].is_object()) {
        const auto& jso = jd["session_options"];
        OnnxModel::Decoder::SessionOptions session_options;

        if (jso.contains("provider_options") && jso["provider_options"].is_array()) {
          for (const auto& entry : jso["provider_options"]) {
            if (entry.is_object()) {
              std::map<std::string, std::string> provider_map;
              for (auto& [key, val] : entry.items()) {
                // Values can be strings or nested objects; flatten to string representation
                if (val.is_string()) {
                  provider_map[key] = val.get<std::string>();
                } else {
                  provider_map[key] = val.dump();
                }
              }
              session_options.provider_options.push_back(std::move(provider_map));
            }
          }
        }

        decoder.session_options = std::move(session_options);
      }

      model.decoder = std::move(decoder);
    }

    config.model = std::move(model);
  }

  if (j.contains("search") && j["search"].is_object()) {
    const auto& js = j["search"];
    Search search;

    if (js.contains("max_length") && js["max_length"].is_number()) {
      search.max_length = js["max_length"].get<int64_t>();
    }

    config.search = std::move(search);
  }

  if (j.contains("engine") && !j["engine"].is_object()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "genai_config.json engine must be an object");
  }

  if (j.contains("engine")) {
    const auto& je = j["engine"];
    Engine engine;

    if (je.contains("static_batching") && !je["static_batching"].is_null()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "genai_config.json engine.static_batching is not supported");
    }

    if (!je.contains("dynamic_batching")) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "genai_config.json engine must contain dynamic_batching");
    }
    if (!je["dynamic_batching"].is_object()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "genai_config.json engine.dynamic_batching must be an object");
    }

    const auto& batching = je["dynamic_batching"];
    Engine::DynamicBatching dynamic_batching;
    dynamic_batching.max_batch_size = ParsePositiveSize(
        batching, "engine.dynamic_batching", "max_batch_size", dynamic_batching.max_batch_size);
    dynamic_batching.max_scheduled_tokens =
        ParsePositiveSize(batching, "engine.dynamic_batching", "max_scheduled_tokens",
                          dynamic_batching.max_scheduled_tokens);
    engine.dynamic_batching = dynamic_batching;
    config.engine = std::move(engine);
  }

  // hidden_size can appear at the top level or inside model
  if (j.contains("model") && j["model"].is_object()) {
    const auto& jm = j["model"];
    if (jm.contains("hidden_size") && jm["hidden_size"].is_number_integer()) {
      config.hidden_size = jm["hidden_size"].get<int>();
    }
  }

  return config;
}

}  // namespace fl
