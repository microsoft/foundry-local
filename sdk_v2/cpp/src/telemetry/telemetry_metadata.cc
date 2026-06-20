// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/telemetry_metadata.h"

#include "telemetry/invocation_context.h"
#include "telemetry/telemetry_environment.h"
#include "version.h"

#include <array>
#include <cstdint>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <sysinfoapi.h>
#else
#include <sys/utsname.h>
#endif

namespace fl {

namespace {

#ifdef _WIN32
std::string GetWindowsVersion() {
  // GetVersionExA is deprecated and lies for unmanifested apps. The reliable
  // approach is to read the build number directly from kernel32 via
  // RtlGetVersion, or fall back to the OS version registry. For now, use
  // GetVersionEx — the deprecation only affects apps without a manifest, and
  // Foundry Local has a manifest declaring Win10 / Win11 compat.
  OSVERSIONINFOEXA info{};
  info.dwOSVersionInfoSize = sizeof(info);
#pragma warning(suppress : 4996)  // GetVersionExA deprecation
  if (::GetVersionExA(reinterpret_cast<LPOSVERSIONINFOA>(&info)) == 0) {
    return "unknown";
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%lu.%lu.%lu",
                info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
  return std::string(buf);
}

std::string GetCpuArch() {
  SYSTEM_INFO si{};
  ::GetNativeSystemInfo(&si);
  switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: return "amd64";
    case PROCESSOR_ARCHITECTURE_ARM:   return "arm";
    case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
    case PROCESSOR_ARCHITECTURE_IA64:  return "ia64";
    case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
    default:                           return "unknown";
  }
}
#else
struct PosixOsInfo {
  std::string name;
  std::string version;
  std::string arch;
};

PosixOsInfo GetPosixOsInfo() {
  PosixOsInfo out{"unknown", "unknown", "unknown"};
  ::utsname u{};
  if (::uname(&u) == 0) {
    out.name = u.sysname;
    out.version = u.release;
    out.arch = u.machine;
  }
  return out;
}
#endif

}  // namespace

TelemetryMetadata BuildTelemetryMetadata(std::string app_name) {
  TelemetryMetadata m;
  m.app_session_guid = MakeGuidV4Hex();
  m.version = FOUNDRY_LOCAL_VERSION;
  m.app_name = std::move(app_name);
  m.test_mode = TelemetryEnvironment::IsTestingMode();

#ifdef _WIN32
  m.os_name = "Windows";
  m.os_version = GetWindowsVersion();
  m.cpu_arch = GetCpuArch();
#else
  auto info = GetPosixOsInfo();
  m.os_name = info.name;
  m.os_version = info.version;
  m.cpu_arch = info.arch;
#endif

  return m;
}

}  // namespace fl
