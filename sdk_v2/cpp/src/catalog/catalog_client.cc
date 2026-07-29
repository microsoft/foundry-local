// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/catalog_client.h"
#include "utils.h"

#include <foundry_local/foundry_local_c.h>

#include <fmt/format.h>

#include <set>

namespace fl {

std::vector<ModelInfo> FetchAllModelInfosWithCachedModels(
    ICatalogClient& client,
    const std::vector<std::string>& cached_model_ids,
    ILogger& logger) {
  // Step 1: Fetch latest catalog models (existing flow).
  auto result = client.FetchAllModelInfos();

  if (cached_model_ids.empty()) {
    return result;
  }

  // Step 2: Find which cached model IDs are already in the catalog results.
  std::set<std::string> resolved_ids;
  for (const auto& info : result) {
    resolved_ids.insert(info.model_id);
  }

  std::vector<std::string> unresolved_ids;
  for (const auto& id : cached_model_ids) {
    if (resolved_ids.find(id) == resolved_ids.end()) {
      unresolved_ids.push_back(id);
    }
  }

  // Step 3: Look up unresolved IDs from the catalog (older versions, etc.)
  if (!unresolved_ids.empty()) {
    try {
      auto additional = client.FetchModelsByIds(unresolved_ids);
      for (auto& info : additional) {
        resolved_ids.insert(info.model_id);
        result.push_back(std::move(info));
      }
    } catch (const std::exception& ex) {
      logger.Log(LogLevel::Warning,
                 fmt::format("catalog: failed to fetch cached model IDs — {}", ex.what()));
    } catch (...) {
      logger.Log(LogLevel::Warning, "catalog: failed to fetch cached model IDs — unknown error");
    }

    // Step 4: Create basic entries for any IDs still unresolved (BYO models).
    for (const auto& id : unresolved_ids) {
      if (resolved_ids.find(id) != resolved_ids.end()) {
        continue;
      }

      auto [name, version] = Utils::SplitModelNameAndVersion(id);

      ModelInfo info;
      info.model_id = id;
      info.name = name;
      info.alias = name;
      info.uri = "local://" + name;
      info.version = version;
      info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MODEL_PROVIDER_STR] = "Local";
      info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_MODEL_TYPE_STR] = "ONNX";

      result.push_back(std::move(info));
    }
  }

  return result;
}

}  // namespace fl
