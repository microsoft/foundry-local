// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "log_level.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fl {

/// Top-level configuration for Manager.
/// Mirrors the C API's flConfiguration design.
struct Configuration {
  std::string app_name;
  std::optional<std::string> app_data_dir;
  std::optional<std::string> model_cache_dir;
  std::optional<std::string> logs_dir;
  LogLevel log_level = LogLevel::Warning;

  /// Catalog URLs with optional per-catalog filter overrides.
  /// `nullopt` filter means "use the catalog's default filter"; an empty string is a
  /// distinct, valid override value.
  /// Defaults to the Azure Foundry Local Catalog if empty.
  std::vector<std::pair<std::string, std::optional<std::string>>> catalog_urls;

  /// Azure region for the model registry download endpoint
  /// (https://{catalog_region}.api.azureml.ms/modelregistry/...).
  /// Resolves a model's asset_id to a downloadable blob storage URL.
  /// Defaults to "centralus" when not set.
  std::optional<std::string> catalog_region;

  /// Web service endpoints to bind to (e.g. "http://127.0.0.1:0").
  /// Defaults to "http://127.0.0.1:0" (ephemeral port) if empty.
  std::vector<std::string> web_service_endpoints;

  /// URL of an external Foundry Local service. When set, the SDK operates in
  /// client-only mode: the catalog uses only the local disk cache (no network
  /// fetch), and local-only operations (StartWebService, session creation) are
  /// blocked. The user is responsible for remote operations (model load/unload,
  /// inference) via the external service's HTTP endpoints.
  std::optional<std::string> external_service_url;

  /// Additional/undocumented options passed through to the core.
  std::map<std::string, std::string> additional_options;

  /// Validates the configuration and resolves defaults.
  /// Expands `{home}` placeholders and sets default cache directory.
  /// Throws fl::Exception on failure.
  void Validate();
};

}  // namespace fl
