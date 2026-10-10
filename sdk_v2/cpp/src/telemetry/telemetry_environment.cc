// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/telemetry_environment.h"

#include "logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fl {

namespace {

// Mirrors neutron-server's CiEnvironmentVariableNames. Keep this in sync if the
// list there changes — telemetry behavior in CI must match across stacks.
constexpr std::array<const char*, 13> kCiEnvironmentVariableNames = {
    "CI",                                  // Generic CI flag used by many providers
    "TF_BUILD",                            // Azure Pipelines
    "GITHUB_ACTIONS",                      // GitHub Actions
    "GITLAB_CI",                           // GitLab CI
    "CIRCLECI",                            // CircleCI
    "TRAVIS",                              // Travis CI
    "JENKINS_URL",                         // Jenkins
    "CODEBUILD_BUILD_ID",                  // AWS CodeBuild
    "BUILDKITE",                           // Buildkite
    "TEAMCITY_VERSION",                    // TeamCity
    "APPVEYOR",                            // AppVeyor
    "BITBUCKET_BUILD_NUMBER",              // Bitbucket Pipelines
    "SYSTEM_TEAMFOUNDATIONCOLLECTIONURI",  // Azure DevOps
};

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

std::string_view Trim(std::string_view s) {
  auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

std::optional<std::string> ReadEnvironmentValue(const char* name) {
#ifdef _WIN32
  // Env-var values are ASCII for the CI flags we care about; use the Win32 A API
  // to avoid depending on CRT getenv behavior.
  return TelemetryInternal::ReadWindowsEnvironment(
      name, [](const char* variable, char* buffer, uint32_t size) -> std::optional<uint32_t> {
        ::SetLastError(ERROR_SUCCESS);
        const DWORD written = ::GetEnvironmentVariableA(variable, buffer, size);
        if (written == 0) {
          const DWORD error = ::GetLastError();
          if (error != ERROR_SUCCESS && error != ERROR_ENVVAR_NOT_FOUND) {
            return std::nullopt;
          }
        }
        return written;
      });
#else
  const char* value = std::getenv(name);
  const auto bounded = BoundedTelemetryCString(value, kMaxTelemetryEnvironmentLength + 1);
  if (bounded.size() > kMaxTelemetryEnvironmentLength) {
    return std::nullopt;
  }
  return std::string(bounded);
#endif
}

bool IsTruthyEnvironmentValue(const char* name) {
  const auto result = TelemetryEnvironment::TryGetEnv(name);
  return !result || TelemetryEnvironment::IsTruthyValue(*result);
}

}  // namespace

std::string TelemetryEnvironment::GetEnv(const char* name) {
  return TryGetEnv(name).value_or(std::string{});
}

std::optional<std::string> TelemetryEnvironment::TryGetEnv(const char* name) {
  auto result = ReadEnvironmentValue(name);
  if (!result) {
    StderrLogger{}.Log(LogLevel::Warning,
                       "[Telemetry] Environment value rejected (32 KiB limit or inconsistent/unreadable value)");
  }
  return result;
}

bool TelemetryEnvironment::IsTruthyValue(std::string_view value) {
  if (value.size() > kMaxTelemetryEnvironmentLength) {
    return true;
  }

  auto trimmed = Trim(value);
  if (trimmed.empty()) {
    return false;
  }
  return !EqualsIgnoreCase(trimmed, "0") &&
         !EqualsIgnoreCase(trimmed, "false") &&
         !EqualsIgnoreCase(trimmed, "no") &&
         !EqualsIgnoreCase(trimmed, "off");
}

bool TelemetryEnvironment::IsCiEnvironment() {
  for (const char* name : kCiEnvironmentVariableNames) {
    if (IsTruthyEnvironmentValue(name)) {
      return true;
    }
  }
  return false;
}

bool TelemetryEnvironment::IsTelemetryDisabledByEnvVar() {
  return IsTruthyEnvironmentValue("ORT_TELEMETRY_DISABLED");
}

}  // namespace fl
