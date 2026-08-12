// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "utils.h"

#include "exception.h"
#include "logger.h"

#include <onnxruntime_c_api.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ftw.h>
#endif

namespace fl {

namespace {

std::string FormatLocationMessage(const CodeLocation& location, const std::string& message) {
  std::ostringstream out;
  out << location.ToString() << " " << message;

  if (!location.stacktrace.empty()) {
    out << "\nStacktrace:";
    for (const auto& frame : location.stacktrace) {
      out << "\n  " << frame;
    }
  }

  return out.str();
}

}  // namespace

#ifdef _WIN32
// Safe getenv wrapper for MSVC (avoids C4996).
static std::optional<std::string> SafeGetEnv(const char* name) {
  char* buf = nullptr;
  size_t len = 0;
  if (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr) {
    std::string result(buf);
    free(buf);
    return result;
  }

  return std::nullopt;
}
#endif

std::optional<std::string> Utils::GetEnv(const char* name) {
#ifdef _WIN32
  return SafeGetEnv(name);
#else
  const char* value = std::getenv(name);
  return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

std::string Utils::GetHomeDir() {
#ifdef _WIN32
  auto home = GetEnv("USERPROFILE");
  if (home.has_value() && !home->empty()) {
    return *home;
  }

  auto drive = GetEnv("HOMEDRIVE");
  auto path = GetEnv("HOMEPATH");
  if (drive.has_value() && path.has_value() && !drive->empty() && !path->empty()) {
    return *drive + *path;
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "unable to determine home directory");
#else
  const char* home = std::getenv("HOME");
  if (home && home[0] != '\0') {
    return home;
  }

  struct passwd* pw = getpwuid(getuid());
  if (pw && pw->pw_dir) {
    return pw->pw_dir;
  }

  FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "unable to determine home directory");
#endif
}

std::string Utils::GetDefaultAppDataDir(const std::string& app_name) {
  // ~/.{app_name}
  return GetHomeDir() + "/." + app_name;
}

std::string Utils::GetDefaultCacheDir(const std::string& app_name) {
  // {app_data}/cache/models
  return GetDefaultAppDataDir(app_name) + "/cache/models";
}

std::string Utils::GetDefaultLogsDir(const std::string& app_name) {
  // {app_data}/logs
  return GetDefaultAppDataDir(app_name) + "/logs";
}

std::string Utils::ExpandHomePlaceholder(const std::string& path) {
  const std::string placeholder = "{home}";
  auto pos = path.find(placeholder);
  if (pos == std::string::npos) {
    return path;
  }

  std::string result = path;
  result.replace(pos, placeholder.size(), GetHomeDir());
  return result;
}

bool Utils::EnsureDirectoryExists(const std::string& path) {
  if (path.empty()) {
    return false;
  }

#ifdef _WIN32
  // Walk the path creating each level
  std::string current;
  for (size_t i = 0; i < path.size(); ++i) {
    current += path[i];
    if (path[i] == '/' || path[i] == '\\' || i == path.size() - 1) {
      if (current.size() > 1 && !(current.size() == 3 && current[1] == ':')) {
        CreateDirectoryA(current.c_str(), nullptr);
      }
    }
  }
  DWORD attrs = GetFileAttributesA(path.c_str());
  return (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
#else
  // Simple recursive mkdir using system
  std::string cmd = "mkdir -p '" + path + "'";
  int ret = system(cmd.c_str());
  if (ret != 0) {
    return false;
  }

  struct stat st;
  return (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
#endif
}

bool Utils::RemoveDirectoryRecursive(const std::string& path) {
  if (path.empty()) {
    return false;
  }

#ifdef _WIN32
  // Use SHFileOperation for recursive delete
  // Path must be double-null terminated
  std::string from = path;
  from.push_back('\0');
  from.push_back('\0');

  SHFILEOPSTRUCTA op{};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
  return SHFileOperationA(&op) == 0;
#else
  auto remove_cb = [](const char* fpath, const struct stat*, int, struct FTW*) -> int {
    return remove(fpath);
  };
  return nftw(path.c_str(), remove_cb, 64, FTW_DEPTH | FTW_PHYS) == 0;
#endif
}

std::pair<std::string, int> Utils::SplitModelNameAndVersion(const std::string& model_id) {
  auto pos = model_id.rfind(':');
  if (pos == std::string::npos || pos == 0 || pos == model_id.size() - 1) {
    return {model_id, 0};
  }

  auto suffix = model_id.substr(pos + 1);

  // Check that suffix is all digits.
  bool all_digits = !suffix.empty();
  for (char c : suffix) {
    if (c < '0' || c > '9') {
      all_digits = false;
      break;
    }
  }

  if (!all_digits) {
    return {model_id, 0};
  }

  try {
    int version = std::stoi(suffix);
    return {model_id.substr(0, pos), version};
  } catch (...) {
    return {model_id, 0};
  }
}

bool Utils::HasGpuHardwareDevice(const OrtApi& ort_api, const OrtEnv& ort_env, ILogger& logger) {
  size_t num_devices = 0;
  OrtStatus* status = ort_api.GetNumHardwareDevices(&ort_env, &num_devices);
  if (status != nullptr) {
    const char* message = ort_api.GetErrorMessage(status);
    logger.Log(LogLevel::Warning,
               std::string("GetNumHardwareDevices failed: ") + (message ? message : "unknown"));
    ort_api.ReleaseStatus(status);
    return false;
  }

  std::vector<const OrtHardwareDevice*> devices(num_devices);
  if (num_devices > 0) {
    status = ort_api.GetHardwareDevices(&ort_env, devices.data(), devices.size());
    if (status != nullptr) {
      const char* message = ort_api.GetErrorMessage(status);
      logger.Log(LogLevel::Warning,
                 std::string("GetHardwareDevices failed: ") + (message ? message : "unknown"));
      ort_api.ReleaseStatus(status);
      return false;
    }
  }

  return std::any_of(devices.begin(), devices.end(), [&](const OrtHardwareDevice* device) {
    return device != nullptr && ort_api.HardwareDevice_Type(device) == OrtHardwareDeviceType_GPU;
  });
}

void Utils::LogAndThrow(ILogger& logger, const CodeLocation& location, const std::string& message,
                        flErrorCode code) {
  auto formatted = FormatLocationMessage(location, message);
  logger.Log(LogLevel::Error, formatted);
  throw Exception(location, message, code);
}

}  // namespace fl
