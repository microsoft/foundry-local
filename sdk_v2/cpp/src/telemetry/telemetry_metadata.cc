// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/telemetry_metadata.h"

#include "telemetry/device_id.h"
#include "telemetry/invocation_context.h"
#include "telemetry/telemetry_environment.h"
#include "version.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <sysinfoapi.h>
#include <winternl.h>
#else
#include <limits.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
#endif
#endif

namespace fl {

namespace {

#ifdef _WIN32
std::string GetProcessPath() {
  std::array<char, MAX_PATH> path{};
  DWORD length = ::GetModuleFileNameA(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0) {
    return {};
  }
  return std::string(path.data(), length);
}

std::string GetProcessName() {
  auto path = GetProcessPath();
  if (path.empty()) {
    return "unknown";
  }
  return std::filesystem::path(path).filename().string();
}

std::string TrimVersionString(std::string value) {
  while (!value.empty() && (value.back() == '\0' || value.back() == '\n' ||
                            value.back() == '\r' || value.back() == ' ')) {
    value.pop_back();
  }
  return value;
}

std::string QueryVersionString(const std::vector<unsigned char>& data, uint16_t language, uint16_t code_page,
                               const wchar_t* name) {
  wchar_t sub_block[128];
  std::swprintf(sub_block, sizeof(sub_block) / sizeof(sub_block[0]), L"\\StringFileInfo\\%04x%04x\\%ls",
                language, code_page, name);

  void* value = nullptr;
  UINT value_len = 0;
  if (!::VerQueryValueW(data.data(), sub_block, &value, &value_len) || value == nullptr || value_len == 0) {
    return {};
  }

  auto* wide_value = static_cast<const wchar_t*>(value);
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide_value, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) {
    return {};
  }

  std::string out(static_cast<size_t>(needed), '\0');
  const int written = ::WideCharToMultiByte(CP_UTF8, 0, wide_value, -1, out.data(), needed, nullptr, nullptr);
  return written > 0 ? TrimVersionString(std::move(out)) : std::string{};
}

std::string FormatFixedFileVersion(const VS_FIXEDFILEINFO& info) {
  if (info.dwSignature != VS_FFI_SIGNATURE) {
    return {};
  }

  char buf[64];
  std::snprintf(buf, sizeof(buf), "%hu.%hu.%hu.%hu",
                HIWORD(info.dwFileVersionMS), LOWORD(info.dwFileVersionMS),
                HIWORD(info.dwFileVersionLS), LOWORD(info.dwFileVersionLS));
  return std::string(buf);
}

std::string GetHostAppVersion() {
  const auto path = GetProcessPath();
  if (path.empty()) {
    return {};
  }

  DWORD handle = 0;
  const DWORD size = ::GetFileVersionInfoSizeA(path.c_str(), &handle);
  if (size == 0) {
    return {};
  }

  std::vector<unsigned char> data(size);
  if (!::GetFileVersionInfoA(path.c_str(), handle, size, data.data())) {
    return {};
  }

  struct LangAndCodePage {
    WORD language;
    WORD code_page;
  };

  void* translations = nullptr;
  UINT translations_len = 0;
  if (::VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", &translations, &translations_len) &&
      translations != nullptr && translations_len >= sizeof(LangAndCodePage)) {
    const auto* entries = static_cast<const LangAndCodePage*>(translations);
    const size_t count = translations_len / sizeof(LangAndCodePage);
    for (size_t i = 0; i < count; ++i) {
      auto version = QueryVersionString(data, entries[i].language, entries[i].code_page, L"ProductVersion");
      if (!version.empty()) {
        return version;
      }
      version = QueryVersionString(data, entries[i].language, entries[i].code_page, L"FileVersion");
      if (!version.empty()) {
        return version;
      }
    }
  }

  void* fixed_info = nullptr;
  UINT fixed_info_len = 0;
  if (::VerQueryValueW(data.data(), L"\\", &fixed_info, &fixed_info_len) &&
      fixed_info != nullptr && fixed_info_len >= sizeof(VS_FIXEDFILEINFO)) {
    return FormatFixedFileVersion(*static_cast<const VS_FIXEDFILEINFO*>(fixed_info));
  }

  return {};
}

int64_t GetTotalMemoryMB() {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!::GlobalMemoryStatusEx(&status)) {
    return -1;
  }
  return static_cast<int64_t>(status.ullTotalPhys / (1024ULL * 1024ULL));
}

std::string GetWindowsVersion() {
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  auto* ntdll = ::GetModuleHandleW(L"ntdll.dll");
  auto* proc = ntdll ? ::GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
  auto* rtl_get_version = reinterpret_cast<RtlGetVersionFn>(proc);
  if (!rtl_get_version) {
    return "unknown";
  }

  RTL_OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (rtl_get_version(&info) != 0) {
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
std::string GetProcessName() {
#if defined(__linux__)
  std::array<char, PATH_MAX> path{};
  ssize_t length = ::readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (length <= 0) {
    return "unknown";
  }
  return std::filesystem::path(std::string(path.data(), static_cast<size_t>(length))).filename().string();
#elif defined(__APPLE__)
  const char* name = ::getprogname();
  return (name != nullptr && name[0] != '\0') ? std::string(name) : std::string{"unknown"};
#else
  return "unknown";
#endif
}

int64_t GetTotalMemoryMB() {
  long pages = ::sysconf(_SC_PHYS_PAGES);
  long page_size = ::sysconf(_SC_PAGE_SIZE);
  if (pages <= 0 || page_size <= 0) {
    return -1;
  }
  return (static_cast<int64_t>(pages) * static_cast<int64_t>(page_size)) / (1024LL * 1024LL);
}

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

#if defined(__APPLE__)
std::string CfStringToUtf8(CFStringRef value) {
  if (value == nullptr) {
    return {};
  }

  if (const char* c_str = CFStringGetCStringPtr(value, kCFStringEncodingUTF8); c_str != nullptr) {
    return std::string(c_str);
  }

  const CFIndex length = CFStringGetLength(value);
  const CFIndex max_size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  if (max_size <= 1) {
    return {};
  }

  std::string out(static_cast<size_t>(max_size), '\0');
  if (!CFStringGetCString(value, out.data(), max_size, kCFStringEncodingUTF8)) {
    return {};
  }
  out.resize(std::strlen(out.c_str()));
  return out;
}

std::string GetBundleString(CFStringRef key) {
  CFBundleRef bundle = CFBundleGetMainBundle();
  if (bundle == nullptr) {
    return {};
  }

  CFTypeRef value = CFBundleGetValueForInfoDictionaryKey(bundle, key);
  if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID()) {
    return {};
  }

  return CfStringToUtf8(static_cast<CFStringRef>(value));
}

std::string GetHostAppVersion() {
  auto version = GetBundleString(CFSTR("CFBundleShortVersionString"));
  if (!version.empty()) {
    return version;
  }
  return GetBundleString(kCFBundleVersionKey);
}
#else
std::string GetHostAppVersion() {
  return {};
}
#endif
#endif

}  // namespace

TelemetryMetadata BuildTelemetryMetadata(std::string app_name) {
  TelemetryMetadata m;
  m.app_session_guid = GenerateGuidV4();
  m.version = FOUNDRY_LOCAL_VERSION;
  m.app_version = GetHostAppVersion();
  if (m.app_version.empty()) {
    m.app_version = m.version;
  }
  m.app_name = std::move(app_name);

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

ProcessInfo BuildProcessInfo(const TelemetryMetadata& metadata, bool include_device_id_status) {
  ProcessInfo info;
  info.app_name = metadata.app_name;
  info.app_version = metadata.app_version;
  info.os_name = metadata.os_name;
  info.os_version = metadata.os_version;
  info.cpu_arch = metadata.cpu_arch;
  info.process_name = GetProcessName();
  info.device_id_status = include_device_id_status ? TelemetryDeviceId::Instance().GetStatusString() : "Disabled";
  info.cpu_count = static_cast<int32_t>(std::thread::hardware_concurrency());
  info.total_memory_mb = GetTotalMemoryMB();
  return info;
}

}  // namespace fl
