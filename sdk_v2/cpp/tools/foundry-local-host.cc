// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include <foundry_local/foundry_local_c.h>

#include "hosting/bootstrap_descriptor.h"
#include "hosting/host_state.h"
#include "hosting/process_watch.h"
#include "util/scope_guard.h"
#include "version.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace {

namespace hosting = fl::hosting;
using Json = nlohmann::json;
using namespace std::chrono_literals;

std::filesystem::path ExecutablePath() {
#ifdef _WIN32
  std::vector<wchar_t> buffer(512);
  while (buffer.size() <= 32768) {
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetModuleFileNameW");
    }
    if (length < buffer.size()) {
      return std::filesystem::canonical(std::filesystem::path(buffer.data(), buffer.data() + length));
    }

    buffer.resize(buffer.size() * 2);
  }

  throw std::runtime_error("native host executable path is too long");
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buffer(size);
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    throw std::runtime_error("cannot resolve native host executable");
  }

  return std::filesystem::canonical(buffer.data());
#else
  return std::filesystem::canonical("/proc/self/exe");
#endif
}

class Library {
 public:
  explicit Library(const std::filesystem::path& path, bool dependency = false) {
#ifdef _WIN32
    (void)dependency;
    module_ =
        LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module_) {
      throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "load native host artifact");
    }
#else
    module_ = dlopen(path.c_str(), RTLD_NOW | (dependency ? RTLD_GLOBAL : RTLD_LOCAL));
    if (!module_) {
      throw std::runtime_error(dlerror());
    }
#endif
  }

  ~Library() {
    if (module_) {
#ifdef _WIN32
      FreeLibrary(module_);
#else
      dlclose(module_);
#endif
    }
  }

  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;

  const flApi* Api() const {
#ifdef _WIN32
    auto get_api = reinterpret_cast<decltype(&FoundryLocalGetApi)>(GetProcAddress(module_, "FoundryLocalGetApi"));
#else
    auto get_api = reinterpret_cast<decltype(&FoundryLocalGetApi)>(dlsym(module_, "FoundryLocalGetApi"));
#endif
    if (!get_api) {
      throw std::runtime_error("native host runtime does not export its versioned API");
    }

    const auto* api = get_api(2);
    if (!api) {
      throw std::runtime_error("native host runtime does not support the required API");
    }

    return api;
  }

 private:
#ifdef _WIN32
  HMODULE module_ = nullptr;
#else
  void* module_ = nullptr;
#endif
};

void Check(const flApi& api, flStatus* status) {
  if (status) {
    const std::string message = api.Status_GetErrorMessage(status);
    api.Status_Release(status);
    throw std::runtime_error(message);
  }
}

void RunNativeHost(const flApi& api, const hosting::BootstrapDescriptor& descriptor,
                   const std::filesystem::path& bootstrap_path) {
  const auto* configuration_api = api.GetConfigurationApi();
  const auto& configuration = descriptor.configuration;
  flConfiguration* config = nullptr;
  Check(api, configuration_api->Create(configuration.app_name.c_str(), &config));
  fl::ScopeGuard release_config([&]() noexcept { configuration_api->Configuration_Release(config); });
  Check(api, configuration_api->SetAppDataDir(config, configuration.app_data_dir->c_str()));
  Check(api, configuration_api->SetModelCacheDir(config, configuration.model_cache_dir->c_str()));
  Check(api, configuration_api->SetLogsDir(config, configuration.logs_dir->c_str()));
  Check(api, configuration_api->SetDefaultLogLevel(config, static_cast<flLogLevel>(configuration.log_level)));
  Check(api, configuration_api->SetCatalogRegion(config, configuration.catalog_region->c_str()));
  for (const auto& [url, filter] : configuration.catalog_urls) {
    Check(api, configuration_api->AddCatalogUrl(config, url.c_str(), filter ? filter->c_str() : nullptr));
  }
  for (const auto& endpoint : configuration.web_service_endpoints) {
    Check(api, configuration_api->AddWebServiceEndpoint(config, endpoint.c_str()));
  }

  flKeyValuePairs* options = nullptr;
  api.CreateKeyValuePairs(&options);
  fl::ScopeGuard release_options([&]() noexcept { api.KeyValuePairs_Release(options); });
  for (const auto& [key, value] : configuration.additional_options) {
    api.AddKeyValuePair(options, key.c_str(), value.c_str());
  }

  api.AddKeyValuePair(options, "DisableNonessentialTelemetry",
                      configuration.disable_nonessential_telemetry ? "true" : "false");
  api.AddKeyValuePair(options, hosting::kChildHostOption, hosting::EncodeChildHostRole(descriptor.child).c_str());
  Check(api, configuration_api->SetAdditionalOptions(config, options));
  flManager* manager = nullptr;
  Check(api, api.Manager_Create(config, &manager));
  fl::ScopeGuard release_manager([&]() noexcept { api.Manager_Release(manager); });
  hosting::PublishHostReady(bootstrap_path, descriptor.child, hosting::CurrentProcessIdentity());
  while (!api.Manager_IsShutdownRequested(manager)) {
    std::this_thread::sleep_for(20ms);
  }

  // The runtime owns draining and normal shutdown. This joins it and reports any fatal service failure.
  Check(api, api.Manager_Shutdown(manager));
}

int Run(const std::filesystem::path& descriptor_path) {
  try {
    const auto descriptor = hosting::DecodeBootstrapDescriptor(hosting::ReadBootstrapJson(descriptor_path));
    const auto process = hosting::CurrentProcessIdentity();
    hosting::PublishHostProcess(descriptor_path, descriptor.child, process);
    std::cout << "FOUNDRY_HOST_PROCESS "
              << Json{{"pid", process.pid},
                      {"generation", process.generation},
                      {"incarnation", descriptor.child.incarnation}}
                     .dump()
              << '\n'
              << std::flush;
    hosting::ProcessWatch starter(descriptor.child.starter);
    hosting::VerifyBootstrapArtifacts(descriptor, ExecutablePath(), FOUNDRY_LOCAL_NATIVE_BUILD_ID,
                                      FOUNDRY_LOCAL_RUNTIME_IDENTIFIER);
    if (starter.IsExited()) {
      return 0;
    }

    // Parent preloads are not inherited. Keep all three modules until the manager and its workers are gone.
    Library ort(descriptor.ort.path, true);
    Library genai(descriptor.genai.path, true);
    Library native(descriptor.native.path);
    RunNativeHost(*native.Api(), descriptor, descriptor_path);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Foundry Local host failed: %s\n", error.what());
    return 1;
  }
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
  if (argc != 3 || std::wstring_view(argv[1]) != L"--bootstrap") {
#else
int main(int argc, char* argv[]) {
  const bool detach = argc == 4 && std::string_view(argv[3]) == "--detach";
  if ((argc != 3 && !detach) || std::string_view(argv[1]) != "--bootstrap") {
#endif
    std::fprintf(stderr,
                 "This executable is started by the Foundry Local SDK using its private bootstrap descriptor.\n");
    return 2;
  }

#ifndef _WIN32
  if (detach) {
    // Fork only in this dependency-light executable, before native modules, threads or manager state exist.
    const auto child = fork();
    if (child < 0) {
      std::perror("Foundry Local host fork");
      return 1;
    }
    if (child > 0) {
      return 0;
    }
    if (setsid() < 0) {
      _exit(1);
    }

    const auto daemon = fork();
    if (daemon < 0) {
      _exit(1);
    }
    if (daemon > 0) {
      _exit(0);
    }
  }
#endif
  return Run(std::filesystem::path(argv[2]));
}
