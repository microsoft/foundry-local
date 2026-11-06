// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "hosting/item_codec.h"
#include "inferencing/session/request.h"
#include "inferencing/session/response.h"
#include "model_info.h"

namespace fl {
struct RequestBudget;
enum class CatalogType;
namespace api {
class ModelBackend;
}
}  // namespace fl

namespace fl::hosting {

struct ModelSelection {
  CatalogType catalog;
  ModelInfo info;
  std::string path;
};

nlohmann::json EncodeModelInfo(const ModelInfo& info);
ModelInfo DecodeModelInfo(const nlohmann::json& value);
nlohmann::json EncodeModelSelection(const ModelSelection& selection);
nlohmann::json EncodeModelSelection(CatalogType catalog, const api::ModelBackend& model);
ModelSelection DecodeModelSelection(const nlohmann::json& value);

nlohmann::json EncodeOptions(const KeyValuePairs& options);
KeyValuePairs DecodeOptions(const nlohmann::json& value);
nlohmann::json EncodeToolDefinition(const ToolDefinition& definition);
ToolDefinition DecodeToolDefinition(const nlohmann::json& value);
nlohmann::json EncodeRequest(const Request& request, const ItemEncodingContext& context);
Request DecodeRequest(const nlohmann::json& value, const ItemDecodingContext& context);
nlohmann::json EncodeResponse(const Response& response, const ItemEncodingContext& context);
Response DecodeResponse(const nlohmann::json& value, const ItemDecodingContext& context);
nlohmann::json EncodeBudget(const RequestBudget& budget);
RequestBudget DecodeBudget(const nlohmann::json& value);

}  // namespace fl::hosting
