// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "configuration.h"
#include "exception.h"
#include "utils.h"

#include <functional>

namespace fl {

namespace {

/// Replace all occurrences of `placeholder` with `value` in `str`.
void ReplacePlaceholder(std::string& str, const std::string& placeholder, const std::string& value) {
  size_t pos = 0;
  while ((pos = str.find(placeholder, pos)) != std::string::npos) {
    str.replace(pos, placeholder.size(), value);
    pos += value.size();
  }
}

/// Expand {home}, {appname}, and {appdata} placeholders in a path.
/// home_resolver is called lazily — only if {home} actually appears.
std::string ExpandPlaceholders(const std::string& path,
                               const std::function<std::string()>& home_resolver,
                               const std::string& app_name,
                               const std::string& app_data_dir) {
  std::string result = path;
  if (result.find("{home}") != std::string::npos) {
    ReplacePlaceholder(result, "{home}", home_resolver());
  }
  ReplacePlaceholder(result, "{appname}", app_name);
  ReplacePlaceholder(result, "{appdata}", app_data_dir);
  return result;
}

}  // namespace

void Configuration::Validate() {
  if (app_name.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Configuration: app_name must not be empty");
  }

  // Validate catalog URLs are non-empty strings if present
  for (const auto& source : catalog_urls) {
    if (source.url.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Configuration: catalog URL must not be empty");
    }
    if (source.name.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Configuration: catalog name must not be empty");
    }
  }

  // Validate web service endpoints are non-empty strings if present
  for (const auto& endpoint : web_service_endpoints) {
    if (endpoint.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Configuration: web service endpoint must not be empty");
    }
  }

  // Resolve defaults and expand placeholders.
  // Order matters: app_data_dir is resolved first so it can be used in {appdata} expansion.
  //
  // GetHomeDir() is resolved lazily — it's only called if a path actually contains the {home} placeholder
  // or if app_data_dir is unset (the default app_data_dir is $HOME/.{app_name}). On Android, $HOME may not
  // be set (e.g. when using adb + emulator for tests), so callers must provide an explicit app_data_dir.
  std::string home_cache;
  auto get_home = [&home_cache]() -> const std::string& {
    if (home_cache.empty()) {
      home_cache = Utils::GetHomeDir();
    }
    return home_cache;
  };

  std::string resolved_app_data;

  if (app_data_dir.has_value()) {
    std::string val = *app_data_dir;
    if (val.find("{home}") != std::string::npos) {
      ReplacePlaceholder(val, "{home}", get_home());
    }
    ReplacePlaceholder(val, "{appname}", app_name);
    resolved_app_data = val;
  } else {
    resolved_app_data = Utils::GetDefaultAppDataDir(app_name);
  }

  app_data_dir = resolved_app_data;

  // Default logs and cache dirs derive from the already-resolved app_data_dir, not from GetHomeDir() —
  // so they work even when $HOME is unavailable.
  logs_dir = logs_dir.has_value()
                 ? ExpandPlaceholders(*logs_dir, get_home, app_name, resolved_app_data)
                 : resolved_app_data + "/logs";

  model_cache_dir = model_cache_dir.has_value()
                        ? ExpandPlaceholders(*model_cache_dir, get_home, app_name, resolved_app_data)
                        : resolved_app_data + "/cache/models";
}

}  // namespace fl
