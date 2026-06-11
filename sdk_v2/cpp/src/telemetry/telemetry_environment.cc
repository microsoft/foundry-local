// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/telemetry_environment.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <vector>
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

}  // namespace

std::string TelemetryEnvironment::GetEnv(const char* name) {
#ifdef _WIN32
  // Use the W variant so we don't depend on the legacy CRT _CRT_SECURE_NO_WARNINGS.
  // Env-var values are ASCII for the CI flags we care about; if a value is unicode
  // we still get the bytes round-tripped correctly because we only do truthiness checks.
  DWORD needed = ::GetEnvironmentVariableA(name, nullptr, 0);
  if (needed == 0) {
    return {};
  }
  std::vector<char> buf(needed);
  DWORD written = ::GetEnvironmentVariableA(name, buf.data(), needed);
  if (written == 0 || written >= needed) {
    return {};
  }
  return std::string(buf.data(), written);
#else
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string{};
#endif
}

bool TelemetryEnvironment::IsTruthyValue(std::string_view value) {
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
    if (IsTruthyValue(GetEnv(name))) {
      return true;
    }
  }
  return false;
}

bool TelemetryEnvironment::IsTestingMode() {
  return IsTruthyValue(GetEnv("FOUNDRY_TESTING_MODE"));
}

}  // namespace fl
