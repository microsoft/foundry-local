// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/host_profile.h"

#include "exception.h"
#include "telemetry/telemetry_environment.h"
#include "util/sha256.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string_view>
#include <utility>

namespace fl::hosting {
namespace {

using Json = nlohmann::json;

std::string NormalizePath(const std::string& value) {
  auto path = std::filesystem::weakly_canonical(std::filesystem::absolute(value));
  while (path.has_relative_path() && path.filename().empty()) {
    path = path.parent_path();
  }

  return path.string();
}

bool IsTruthy(std::string_view value) {
  const auto lowered = ToLower(std::string(value));
  return lowered == "true" || lowered == "1" || lowered == "yes";
}

Json RuntimeCompatibility(const RuntimeIdentity& runtime) {
  static constexpr std::array<std::string_view, 5> supported{"win-x64", "win-arm64", "linux-x64", "linux-arm64",
                                                             "osx-arm64"};
  if (runtime.native_build.empty() || runtime.ipc_protocol_version == 0 ||
      std::find(supported.begin(), supported.end(), runtime.runtime_identifier) == supported.end()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared-host runtime identity is missing or unsupported");
  }

  return {{"native_build", runtime.native_build},
          {"rid", runtime.runtime_identifier},
          {"protocol", runtime.ipc_protocol_version}};
}

}  // namespace

HostProfile BuildHostProfile(const Configuration& configuration, const RuntimeIdentity& runtime) {
  HostProfile profile;
  profile.configuration = configuration;
  auto& config = profile.configuration;
  config.Validate();
  if (config.external_service_url) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared hosting cannot use external-service client-only mode");
  }

  config.app_data_dir = NormalizePath(*config.app_data_dir);
  config.model_cache_dir = NormalizePath(*config.model_cache_dir);
  config.logs_dir = NormalizePath(*config.logs_dir);
  config.catalog_region = config.catalog_region.value_or("auto");
  if (config.catalog_urls.empty()) {
    config.catalog_urls.emplace_back(Configuration::kDefaultCatalogUrl,
                                     std::string(Configuration::kDefaultCatalogFilter));
  }
  if (config.web_service_endpoints.empty()) {
    config.web_service_endpoints.emplace_back("http://127.0.0.1:0");
  }

  auto options = config.additional_options;
  options.erase("UserAgent");
  const auto telemetry_option = options.find("DisableNonessentialTelemetry");
  const bool disable_nonessential = config.disable_nonessential_telemetry ||
                                    (telemetry_option != options.end() && IsTruthy(telemetry_option->second)) ||
                                    TelemetryEnvironment::IsTelemetryDisabledByEnvVar();
  options.erase("DisableNonessentialTelemetry");

  Json catalogs = Json::array();
  for (const auto& [url, filter] : config.catalog_urls) {
    catalogs.push_back({{"url", url}, {"filter", filter ? Json(*filter) : Json(nullptr)}});
  }

  profile.compatibility = {{"runtime", RuntimeCompatibility(runtime)},
                           {"profile",
                            {{"app_data_dir", *config.app_data_dir},
                             {"model_cache_dir", *config.model_cache_dir},
                             {"logs_dir", *config.logs_dir},
                             {"log_level", static_cast<int>(config.log_level)},
                             {"catalogs", std::move(catalogs)},
                             {"catalog_region", *config.catalog_region},
                             {"http_endpoints", config.web_service_endpoints},
                             {"execution_options", std::move(options)},
                             {"disable_nonessential_telemetry", disable_nonessential},
                             {"telemetry_hard_disabled", TelemetryEnvironment::IsCiEnvironment()}}}};
  profile.key = Sha256String(profile.compatibility.dump());
  return profile;
}

}  // namespace fl::hosting
