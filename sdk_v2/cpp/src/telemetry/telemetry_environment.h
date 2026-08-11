// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <string>
#include <string_view>

namespace fl {

/// Static helpers for telemetry runtime gating. Ported from neutron-server's
/// TelemetryEnvironment.cs so the CI suppression behavior matches across stacks.
class TelemetryEnvironment {
 public:
  /// Returns true if any well-known CI environment variable is set to a truthy
  /// value. The set matches neutron-server's TelemetryEnvironment.cs.
  /// In CI, OneDsTelemetry skips Initialize entirely — no 1DS events emitted.
  static bool IsCiEnvironment();

  /// Returns true when the shared ORT telemetry opt-out environment variable is set.
  static bool IsTelemetryDisabledByEnvVar();

  /// Truthy-value semantics: a non-empty, non-whitespace string whose trimmed
  /// value is not "0", "false", "no", or "off" (case-insensitive).
  static bool IsTruthyValue(std::string_view value);

  /// Read an env var (cross-platform). Returns empty string if unset.
  static std::string GetEnv(const char* name);
};

}  // namespace fl
