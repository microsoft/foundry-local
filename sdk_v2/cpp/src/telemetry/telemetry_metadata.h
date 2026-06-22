// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <string>

namespace fl {

/// Process-wide metadata stamped onto every 1DS event as common context.
/// Computed once at startup and cached. Cheap to copy.
struct TelemetryMetadata {
  /// Hex-encoded random 128-bit GUID, generated once at startup. Stamped on
  /// every event as `AppSessionGuid` so the backend can group all events from a
  /// single FL process run. This is a stable per-process correlation id and is
  /// distinct from the SDK's rotating usage-session id (ext.app.sesId), which is
  /// driven separately via LogSession(Started/Ended).
  std::string app_session_guid;

  /// Foundry Local SDK version (FoundryLocalGetVersionString).
  std::string version;

  /// Configured app name (from Configuration::app_name).
  std::string app_name;

  /// Free-form "Windows 11 10.0.26100 amd64" / "Linux 6.5.0 x86_64" / "macOS 14.4 arm64".
  std::string os_name;       // "Windows" / "Linux" / "Darwin"
  std::string os_version;    // "10.0.26100" / "6.5.0-azure" / "14.4"
  std::string cpu_arch;      // "amd64" / "arm64" / "x86" / ...

  /// True if FOUNDRY_TESTING_MODE was truthy at startup (stamped per-event as `test`).
  bool test_mode = false;
};

/// Build the metadata for this process. Reads env vars and OS APIs once.
/// app_name comes from Configuration; version comes from FoundryLocalGetVersionString.
TelemetryMetadata BuildTelemetryMetadata(std::string app_name);

}  // namespace fl
