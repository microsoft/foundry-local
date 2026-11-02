// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/sdk_values.h"

#include "api/backend.h"
#include "catalog.h"
#include "exception.h"
#include "inferencing/generative/chat/chat_generator.h"
#include "inferencing/generative/chat/search_options.h"

#include <limits>
#include <map>
#include <type_traits>

namespace fl::hosting {
namespace {

using Json = nlohmann::json;

template <typename F>
auto DecodeChecked(F&& decode) {
  try {
    return decode();
  } catch (const Json::exception&) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid SDK wire value");
  }
}

template <typename T>
T Integer(const Json& value) {
  static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
  if (value.is_number_unsigned()) {
    const auto number = value.get<uint64_t>();
    if (number <= static_cast<uint64_t>(std::numeric_limits<T>::max())) {
      return static_cast<T>(number);
    }
  } else if (value.is_number_integer()) {
    const auto number = value.get<int64_t>();
    if constexpr (std::is_signed_v<T>) {
      if (number >= std::numeric_limits<T>::min() && number <= std::numeric_limits<T>::max()) {
        return static_cast<T>(number);
      }
    } else {
      if (number >= 0 && static_cast<uint64_t>(number) <= std::numeric_limits<T>::max()) {
        return static_cast<T>(number);
      }
    }
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid SDK wire integer");
}

ToolKind DecodeToolKind(const Json& value) {
  switch (Integer<int>(value)) {
    case static_cast<int>(ToolKind::kFunction):
      return ToolKind::kFunction;
    case static_cast<int>(ToolKind::kCustom):
      return ToolKind::kCustom;
    default:
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid SDK wire tool kind");
  }
}

Json EncodeItems(const std::vector<Item*>& items, const ItemEncodingContext& context) {
  Json values = Json::array();
  for (const auto* item : items) {
    if (!item) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "SDK request contains a null item");
    }

    values.push_back(EncodeItem(*item, context));
  }

  return values;
}

void RequireArray(const Json& value) {
  if (!value.is_array()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "SDK wire collection must be an array");
  }
}

}  // namespace

Json EncodeModelInfo(const ModelInfo& info) {
  return {{"id", info.model_id},
          {"name", info.name},
          {"version", info.version},
          {"alias", info.alias},
          {"uri", info.uri},
          {"device", static_cast<int>(info.device_type)},
          {"ep", info.execution_provider},
          {"task", info.task},
          {"region", info.detected_region},
          {"templates", EncodeOptions(info.prompt_templates)},
          {"settings", EncodeOptions(info.model_settings)},
          {"strings", info.string_properties},
          {"integers", info.int_properties}};
}

ModelInfo DecodeModelInfo(const Json& value) {
  return DecodeChecked([&] {
    ModelInfo info;
    info.model_id = value.at("id").get<std::string>();
    info.name = value.at("name").get<std::string>();
    info.version = Integer<int>(value.at("version"));
    info.alias = value.at("alias").get<std::string>();
    info.uri = value.at("uri").get<std::string>();
    const auto device = Integer<int>(value.at("device"));
    if (device < static_cast<int>(DeviceType::kNotSet) || device > static_cast<int>(DeviceType::kNPU)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid SDK model device");
    }

    info.device_type = static_cast<DeviceType>(device);
    info.execution_provider = value.at("ep").get<std::string>();
    info.task = value.at("task").get<std::string>();
    info.detected_region = value.at("region").get<std::string>();
    info.prompt_templates = DecodeOptions(value.at("templates"));
    info.model_settings = DecodeOptions(value.at("settings"));
    const auto strings = DecodeOptions(value.at("strings"));
    info.string_properties.insert(strings.begin(), strings.end());
    if (!value.at("integers").is_object()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid SDK model properties");
    }

    for (const auto& [key, number] : value.at("integers").items()) {
      info.int_properties.emplace(key, Integer<int64_t>(number));
    }

    return info;
  });
}

Json EncodeModelSelection(const ModelSelection& selection) {
  std::string catalog;
  switch (selection.catalog) {
    case CatalogType::kPublic:
      catalog = "public";
      break;
    case CatalogType::kLocal:
      catalog = "local";
      break;
    default:
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid SDK model catalog");
  }

  return {{"catalog", catalog}, {"info", EncodeModelInfo(selection.info)}, {"path", selection.path}};
}

Json EncodeModelSelection(CatalogType catalog, const api::ModelBackend& model) {
  return EncodeModelSelection({catalog, model.Info(), model.GetPath()});
}

ModelSelection DecodeModelSelection(const Json& value) {
  return DecodeChecked([&] {
    const auto catalog = value.at("catalog").get<std::string>();
    if (catalog != "public" && catalog != "local") {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid SDK model catalog");
    }

    return ModelSelection{catalog == "public" ? CatalogType::kPublic : CatalogType::kLocal,
                          DecodeModelInfo(value.at("info")), value.at("path").get<std::string>()};
  });
}

Json EncodeOptions(const KeyValuePairs& options) { return options.Entries(); }

KeyValuePairs DecodeOptions(const Json& value) {
  return DecodeChecked([&] {
    if (!value.is_object()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "SDK wire options must be an object");
    }

    KeyValuePairs options;
    options.CopyFromMap(value.get<std::map<std::string, std::string>>());
    return options;
  });
}

Json EncodeToolDefinition(const ToolDefinition& definition) {
  return {
      {"name", definition.name},
      {"description", definition.description},
      {"schema", definition.json_schema},
      {"kind", static_cast<int>(definition.kind)},
      {"include_description", definition.include_description_in_prompt},
      {"include_parameters", definition.include_parameters_in_prompt},
      {"strict", definition.strict ? Json(*definition.strict) : Json(nullptr)},
      {"custom_lark_grammar", definition.custom_lark_grammar ? Json(*definition.custom_lark_grammar) : Json(nullptr)}};
}

ToolDefinition DecodeToolDefinition(const Json& value) {
  return DecodeChecked([&] {
    ToolDefinition definition(value.at("name").get<std::string>(), value.at("description").get<std::string>(),
                              value.at("schema").get<std::string>(), DecodeToolKind(value.at("kind")),
                              value.at("include_description").get<bool>(), value.at("include_parameters").get<bool>());
    if (!value.at("strict").is_null()) {
      definition.strict = value.at("strict").get<bool>();
    }
    if (!value.at("custom_lark_grammar").is_null()) {
      definition.custom_lark_grammar = value.at("custom_lark_grammar").get<std::string>();
    }

    return definition;
  });
}

Json EncodeRequest(const Request& request, const ItemEncodingContext& context) {
  Json value{{"items", EncodeItems(request.items, context)},
             {"options", EncodeOptions(request.options)},
             {"segments", request.item_segment_starts},
             {"prepared_tools", nullptr},
             {"forced_tool", nullptr},
             {"raw_envelope", nullptr}};
  if (request.prepared_tool_definitions) {
    value["prepared_tools"] = Json::array();
    for (const auto& definition : *request.prepared_tool_definitions) {
      value["prepared_tools"].push_back(EncodeToolDefinition(definition));
    }
  }
  if (request.forced_tool_choice) {
    value["forced_tool"] = {{"name", request.forced_tool_choice->name},
                            {"kind", static_cast<int>(request.forced_tool_choice->kind)}};
  }
  if (request.raw_envelope_descriptor) {
    const auto& envelope = *request.raw_envelope_descriptor;
    value["raw_envelope"] = {
        {"tool", envelope.tool_name}, {"start", envelope.start_marker}, {"end", envelope.end_marker}};
  }

  return value;
}

Request DecodeRequest(const Json& value, const ItemDecodingContext& context) {
  return DecodeChecked([&] {
    Request request;
    request.options = DecodeOptions(value.at("options"));
    RequireArray(value.at("items"));
    for (const auto& item : value.at("items")) {
      request.AddOwnedItem(DecodeItem(item, context));
    }

    RequireArray(value.at("segments"));
    for (const auto& segment : value.at("segments")) {
      const auto index = Integer<size_t>(segment);
      if (!request.item_segment_starts.empty() && index < request.item_segment_starts.back()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "SDK wire replay segments must be ordered");
      }

      request.item_segment_starts.push_back(index);
    }

    if (!value.at("prepared_tools").is_null()) {
      RequireArray(value.at("prepared_tools"));
      request.prepared_tool_definitions.emplace();
      for (const auto& definition : value.at("prepared_tools")) {
        request.prepared_tool_definitions->push_back(DecodeToolDefinition(definition));
      }
    }
    if (!value.at("forced_tool").is_null()) {
      const auto& tool = value.at("forced_tool");
      request.forced_tool_choice =
          ForcedToolChoice{tool.at("name").get<std::string>(), DecodeToolKind(tool.at("kind"))};
    }
    if (!value.at("raw_envelope").is_null()) {
      const auto& envelope = value.at("raw_envelope");
      request.raw_envelope_descriptor =
          RawEnvelopeDescriptor{envelope.at("tool").get<std::string>(), envelope.at("start").get<std::string>(),
                                envelope.at("end").get<std::string>()};
    }

    return request;
  });
}

Json EncodeResponse(const Response& response, const ItemEncodingContext& context) {
  Json items = Json::array();
  for (const auto& item : response.items) {
    if (!item) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "SDK response contains a null item");
    }

    items.push_back(EncodeItem(*item, context));
  }

  return {
      {"items", std::move(items)},
      {"finish", static_cast<int>(response.finish_reason)},
      {"termination", response.termination_cause ? Json(static_cast<int>(*response.termination_cause)) : Json(nullptr)},
      {"usage",
       {{"prompt", response.usage.prompt_tokens},
        {"completion", response.usage.completion_tokens},
        {"total", response.usage.total_tokens},
        {"reasoning", response.usage.reasoning_tokens},
        {"cached", response.usage.cached_prompt_tokens}}},
      {"metadata", EncodeOptions(response.metadata)},
      {"json_message_count",
       response.openai_json_message_count ? Json(*response.openai_json_message_count) : Json(nullptr)}};
}

Response DecodeResponse(const Json& value, const ItemDecodingContext& context) {
  return DecodeChecked([&] {
    Response response;
    RequireArray(value.at("items"));
    for (const auto& item : value.at("items")) {
      response.items.push_back(DecodeItem(item, context));
    }

    const auto finish = Integer<int>(value.at("finish"));
    if (finish < FOUNDRY_LOCAL_FINISH_NONE || finish > FOUNDRY_LOCAL_FINISH_TOOL_CALLS) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid SDK wire finish reason");
    }

    response.finish_reason = static_cast<flFinishReason>(finish);
    if (!value.at("termination").is_null()) {
      const auto cause = Integer<int>(value.at("termination"));
      if (cause < static_cast<int>(BackendTerminationCause::kNaturalEnd) ||
          cause > static_cast<int>(BackendTerminationCause::kFailure)) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid SDK wire termination cause");
      }

      response.termination_cause = static_cast<BackendTerminationCause>(cause);
    }

    const auto& usage = value.at("usage");
    response.usage = {Integer<int64_t>(usage.at("prompt")), Integer<int64_t>(usage.at("completion")),
                      Integer<int64_t>(usage.at("total")), Integer<int64_t>(usage.at("reasoning")),
                      Integer<int64_t>(usage.at("cached"))};
    response.metadata = DecodeOptions(value.at("metadata"));
    if (value.at("json_message_count").is_null()) {
      response.openai_json_message_count = std::nullopt;
    } else {
      response.openai_json_message_count = Integer<uint64_t>(value.at("json_message_count"));
    }

    return response;
  });
}

Json EncodeBudget(const RequestBudget& budget) {
  return {{"prompt", budget.prompt_tokens},
          {"output", budget.output_reserve_tokens},
          {"required", budget.required_tokens},
          {"limit", budget.context_limit_tokens},
          {"fits", budget.fits},
          {"deficit", budget.deficit_tokens}};
}

RequestBudget DecodeBudget(const Json& value) {
  return DecodeChecked([&] {
    return RequestBudget{Integer<int64_t>(value.at("prompt")), Integer<int64_t>(value.at("output")),
                         Integer<int64_t>(value.at("required")), Integer<int64_t>(value.at("limit")),
                         value.at("fits").get<bool>(), Integer<int64_t>(value.at("deficit"))};
  });
}

}  // namespace fl::hosting
