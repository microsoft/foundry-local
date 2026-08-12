// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/nvml_gpu_detector.h"

#include "logger.h"

#include <fmt/format.h>

#include <filesystem>

#if defined(FOUNDRY_LOCAL_DESKTOP_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#include <shlobj.h>
#include <windows.h>
#elif !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace fl {

namespace {

using NvmlDevice = void*;
constexpr int kNvmlSuccess = 0;

using NvmlInitFn = int (*)();
using NvmlShutdownFn = int (*)();
using NvmlDeviceGetCountFn = int (*)(unsigned int*);
using NvmlDeviceGetHandleByIndexFn = int (*)(unsigned int, NvmlDevice*);
using NvmlDeviceGetCudaComputeCapabilityFn = int (*)(NvmlDevice, int*, int*);

#if defined(FOUNDRY_LOCAL_DESKTOP_WINDOWS)

using LibraryHandle = HMODULE;
constexpr LibraryHandle kNullLibrary = nullptr;

LibraryHandle LoadNvmlFromProgramFiles(REFKNOWNFOLDERID folder_id) {
  PWSTR program_files = nullptr;
  if (SHGetKnownFolderPath(folder_id, KF_FLAG_DEFAULT, nullptr, &program_files) != S_OK) {
    return nullptr;
  }

  const std::filesystem::path path =
      std::filesystem::path(program_files) / L"NVIDIA Corporation" / L"NVSMI" / L"nvml.dll";
  CoTaskMemFree(program_files);
  return LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

LibraryHandle LoadNvmlLibrary() {
  if (auto library = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
    return library;
  }

  if (auto library = LoadNvmlFromProgramFiles(FOLDERID_ProgramFiles)) {
    return library;
  }

#if defined(_M_ARM64)
  return nullptr;
#else
  return LoadNvmlFromProgramFiles(FOLDERID_ProgramFilesX64);
#endif
}

void* GetSymbol(LibraryHandle lib, const char* name) { return reinterpret_cast<void*>(GetProcAddress(lib, name)); }

void UnloadLibrary(LibraryHandle lib) {
  if (lib) {
    FreeLibrary(lib);
  }
}

#elif defined(_WIN32)

using LibraryHandle = void*;
constexpr LibraryHandle kNullLibrary = nullptr;

LibraryHandle LoadNvmlLibrary() { return nullptr; }

void* GetSymbol(LibraryHandle /*lib*/, const char* /*name*/) { return nullptr; }

void UnloadLibrary(LibraryHandle /*lib*/) {}

#else

using LibraryHandle = void*;
constexpr LibraryHandle kNullLibrary = nullptr;

LibraryHandle LoadNvmlLibrary() { return dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL); }

void* GetSymbol(LibraryHandle lib, const char* name) { return dlsym(lib, name); }

void UnloadLibrary(LibraryHandle lib) {
  if (lib) {
    dlclose(lib);
  }
}

#endif

class NvmlLibrary {
 public:
  explicit NvmlLibrary(ILogger& logger) {
    lib_ = LoadNvmlLibrary();
    if (!lib_) {
      return;
    }

    init_ = reinterpret_cast<NvmlInitFn>(GetSymbol(lib_, "nvmlInit_v2"));
    shutdown_ = reinterpret_cast<NvmlShutdownFn>(GetSymbol(lib_, "nvmlShutdown"));
    get_count_ = reinterpret_cast<NvmlDeviceGetCountFn>(GetSymbol(lib_, "nvmlDeviceGetCount_v2"));
    get_handle_ = reinterpret_cast<NvmlDeviceGetHandleByIndexFn>(GetSymbol(lib_, "nvmlDeviceGetHandleByIndex_v2"));
    get_compute_cap_ =
        reinterpret_cast<NvmlDeviceGetCudaComputeCapabilityFn>(GetSymbol(lib_, "nvmlDeviceGetCudaComputeCapability"));

    if (!init_ || !shutdown_ || !get_count_ || !get_handle_ || !get_compute_cap_) {
      logger.Log(LogLevel::Warning, "NVML library is missing required symbols; CUDA detection is unavailable");
      UnloadLibrary(lib_);
      lib_ = kNullLibrary;
      return;
    }

    const auto init_result = init_();
    initialized_ = (init_result == kNvmlSuccess);
    if (!initialized_) {
      logger.Log(LogLevel::Warning,
                 fmt::format("NVML initialization failed with error {}; CUDA detection is unavailable", init_result));
    }
  }

  ~NvmlLibrary() {
    if (initialized_ && shutdown_) {
      shutdown_();
    }
    UnloadLibrary(lib_);
  }

  NvmlLibrary(const NvmlLibrary&) = delete;
  NvmlLibrary& operator=(const NvmlLibrary&) = delete;

  bool IsReady() const { return lib_ != kNullLibrary && initialized_; }

  std::vector<std::pair<int, int>> QueryComputeCapabilities() const {
    std::vector<std::pair<int, int>> result;
    if (!IsReady()) {
      return result;
    }

    unsigned int count = 0;
    if (get_count_(&count) != kNvmlSuccess || count == 0) {
      return result;
    }

    for (unsigned int i = 0; i < count; ++i) {
      NvmlDevice device = nullptr;
      if (get_handle_(i, &device) != kNvmlSuccess) {
        continue;
      }

      int major = 0;
      int minor = 0;
      if (get_compute_cap_(device, &major, &minor) == kNvmlSuccess) {
        result.emplace_back(major, minor);
      }
    }

    return result;
  }

 private:
  LibraryHandle lib_ = kNullLibrary;
  bool initialized_ = false;
  NvmlInitFn init_ = nullptr;
  NvmlShutdownFn shutdown_ = nullptr;
  NvmlDeviceGetCountFn get_count_ = nullptr;
  NvmlDeviceGetHandleByIndexFn get_handle_ = nullptr;
  NvmlDeviceGetCudaComputeCapabilityFn get_compute_cap_ = nullptr;
};

}  // namespace

bool HasQualifyingComputeCapability(const std::vector<std::pair<int, int>>& capabilities, int min_major,
                                    int min_minor) {
  for (const auto& [major, minor] : capabilities) {
    if (major > min_major || (major == min_major && minor >= min_minor)) {
      return true;
    }
  }

  return false;
}

bool NvmlGpuDetector::HasNvidiaGpu(ILogger& logger) {
  NvmlLibrary nvml(logger);
  if (!nvml.IsReady()) {
    return false;
  }

  return HasQualifyingComputeCapability(nvml.QueryComputeCapabilities());
}

}  // namespace fl
