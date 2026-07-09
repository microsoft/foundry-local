// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <foundry_local/foundry_local_c.h>

#include "exception.h"
#include "util/string_utils.h"

#include <optional>
#include <string>
#include <utility>

namespace fl {

class ILogger;

struct Utils {
  /// Read an environment variable value. Returns nullopt when missing.
  /// Empty string is a valid value when the variable exists but has no content.
  static std::optional<std::string> GetEnv(const char* name);

  /// Get the user's home directory.
  static std::string GetHomeDir();

  /// Get the default app data directory: ~/.{app_name}
  static std::string GetDefaultAppDataDir(const std::string& app_name);

  /// Get the default model cache directory: {app_data}/cache/models
  static std::string GetDefaultCacheDir(const std::string& app_name);

  /// Get the default logs directory: {app_data}/logs
  static std::string GetDefaultLogsDir(const std::string& app_name);

  /// Expand {home} placeholder in a path string with the actual home directory.
  static std::string ExpandHomePlaceholder(const std::string& path);

  /// Recursively remove a directory and all its contents. Returns true on success.
  static bool RemoveDirectoryRecursive(const std::string& path);

  /// Ensure a directory exists, creating it and parents if needed. Returns true on success.
  static bool EnsureDirectoryExists(const std::string& path);

  /// Split a model ID like "model-name:3" into name and version.
  /// Returns {name, version}. If no version suffix, returns {model_id, 0}.
  static std::pair<std::string, int> SplitModelNameAndVersion(const std::string& model_id);

  /// Log an error message and throw fl::Exception.
  /// Use with fmt::format at the call site:
  ///   FL_LOG_AND_THROW(logger, "failed to load model ", id, ": ", e.what());
  [[noreturn]] static void LogAndThrow(ILogger& logger, const CodeLocation& location, const std::string& message,
                                       flErrorCode code);

  /// Convert a flMessageRole enum to its string representation (e.g. "user", "assistant").
  static const char* RoleToString(flMessageRole role) {
    switch (role) {
      case FOUNDRY_LOCAL_ROLE_SYSTEM:
        return "system";
      case FOUNDRY_LOCAL_ROLE_USER:
        return "user";
      case FOUNDRY_LOCAL_ROLE_ASSISTANT:
        return "assistant";
      case FOUNDRY_LOCAL_ROLE_TOOL:
        return "tool";
      case FOUNDRY_LOCAL_ROLE_DEVELOPER:
        return "developer";
      default:
        return "user";
    }
  }

  /// Convert a role string (e.g. "user", "assistant") to a flMessageRole enum.
  static flMessageRole StringToRole(const std::string& role) {
    if (role == "system") return FOUNDRY_LOCAL_ROLE_SYSTEM;
    if (role == "user") return FOUNDRY_LOCAL_ROLE_USER;
    if (role == "assistant") return FOUNDRY_LOCAL_ROLE_ASSISTANT;
    if (role == "tool") return FOUNDRY_LOCAL_ROLE_TOOL;
    if (role == "developer") return FOUNDRY_LOCAL_ROLE_DEVELOPER;
    return FOUNDRY_LOCAL_ROLE_NONE;
  }
};

#define FL_LOG_AND_THROW(logger, code, ...) \
  ::fl::Utils::LogAndThrow((logger), FL_WHERE_WITH_STACK, ::fl::MakeString(__VA_ARGS__), (code))

}  // namespace fl
