// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog/catalog_client.h"
#include "ep_detection/ep_detector.h"
#include "telemetry/telemetry.h"
#include "telemetry/telemetry_redaction.h"
#include "utils.h"

#include <foundry_local/foundry_local_c.h>

#include <fmt/format.h>

#include <chrono>
#include <memory>
#include <set>

namespace fl {

namespace {

constexpr const char* kCatalogFetchFailure = "catalog request failed";

}  // namespace

std::vector<ModelInfo> FetchAllModelInfosWithCachedModels(
    ICatalogClient& client,
    const std::vector<std::string>& cached_model_ids,
    ILogger& logger,
    ITelemetry* telemetry,
    const CatalogFetchInfo* base_info) {
  auto now = [] { return std::chrono::steady_clock::now(); };
  auto elapsed_ms = [](std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
        .count();
  };
  auto emit = [&](const std::string& operation, ActionStatus status, int64_t duration_ms,
                  int32_t model_count, const std::string& error) {
    if (telemetry == nullptr || base_info == nullptr) {
      return;
    }
    CatalogFetchInfo info = *base_info;
    info.operation = operation;
    info.status = status;
    info.duration_ms = duration_ms;
    info.model_count = model_count;
    info.error_message = error;
    telemetry->RecordCatalogFetch(info);
  };

  // Step 1: Fetch latest catalog models (the primary catalog access).
  std::vector<ModelInfo> result;
  {
    const auto start = now();
    try {
      result = client.FetchAllModelInfos();
    } catch (const std::exception& ex) {
      logger.Log(LogLevel::Warning,
                 fmt::format("catalog: failed to fetch models — {}", ScrubTelemetryErrorMessage(ex.what())));
      emit("FetchAll", ActionStatus::kFailure, elapsed_ms(start), 0, kCatalogFetchFailure);
      throw;
    }
    emit("FetchAll", ActionStatus::kSuccess, elapsed_ms(start), static_cast<int32_t>(result.size()), "");
  }

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
    const auto start = now();
    try {
      auto additional = client.FetchModelsByIds(unresolved_ids);
      const auto additional_count = static_cast<int32_t>(additional.size());
      for (auto& info : additional) {
        resolved_ids.insert(info.model_id);
        result.push_back(std::move(info));
      }
      emit("FetchByIds", ActionStatus::kSuccess, elapsed_ms(start), additional_count, "");
    } catch (const std::exception& ex) {
      auto error_message = ScrubTelemetryErrorMessage(ex.what());
      logger.Log(LogLevel::Warning,
                 fmt::format("catalog: failed to fetch cached model IDs — {}", error_message));
      emit("FetchByIds", ActionStatus::kFailure, elapsed_ms(start), 0, kCatalogFetchFailure);
    } catch (...) {
      logger.Log(LogLevel::Warning, "catalog: failed to fetch cached model IDs — unknown error");
      emit("FetchByIds", ActionStatus::kFailure, elapsed_ms(start), 0, "unknown error");
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
