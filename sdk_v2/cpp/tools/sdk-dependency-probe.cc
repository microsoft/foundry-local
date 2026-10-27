// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <foundry_local/foundry_local_c.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;

double Milliseconds(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void Check(const flApi& api, flStatus* status) {
  if (status == nullptr) return;
  std::string message = api.Status_GetErrorMessage(status);
  api.Status_Release(status);
  throw std::runtime_error(message);
}
}  // namespace

int main(int argc, char* argv[]) {
  try {
    if (argc != 3 && argc != 4) {
      throw std::runtime_error("Expected: sdk-dependency-probe <library> <isolated-home> [model-id]");
    }
    const auto library = std::filesystem::absolute(argv[1]);
    const auto home = std::filesystem::absolute(argv[2]);
    std::filesystem::create_directories(home);
    const auto load_start = Clock::now();
#ifdef _WIN32
    auto module = LoadLibraryExW(library.c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr) throw std::runtime_error("Cannot load SDK: " + std::to_string(GetLastError()));
    auto get_api = reinterpret_cast<decltype(&FoundryLocalGetApi)>(GetProcAddress(module, "FoundryLocalGetApi"));
#else
    auto module = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (module == nullptr) throw std::runtime_error(dlerror());
    auto get_api = reinterpret_cast<decltype(&FoundryLocalGetApi)>(dlsym(module, "FoundryLocalGetApi"));
#endif
    if (get_api == nullptr) throw std::runtime_error("SDK does not export FoundryLocalGetApi");
    const auto* api = get_api(FOUNDRY_LOCAL_API_VERSION);
    if (api == nullptr) throw std::runtime_error("SDK does not support the probe's ABI");
    const double load_ms = Milliseconds(load_start);
    const auto* config_api = api->GetConfigurationApi();
    flConfiguration* config = nullptr;
    Check(*api, config_api->Create("dependency-benchmark", &config));
    Check(*api, config_api->SetAppDataDir(config, home.string().c_str()));
    Check(*api, config_api->SetModelCacheDir(config, (home / "models").string().c_str()));
    Check(*api, config_api->SetLogsDir(config, (home / "logs").string().c_str()));
    Check(*api, config_api->SetDefaultLogLevel(config, FOUNDRY_LOCAL_LOG_ERROR));
    const auto create_start = Clock::now();
    flManager* manager = nullptr;
    Check(*api, api->Manager_Create(config, &manager));
    const double create_ms = Milliseconds(create_start);
    config_api->Configuration_Release(config);

    double download_ms = 0;
    uintmax_t download_bytes = 0;
    if (argc == 4) {
      flCatalog* catalog = nullptr;
      flModel* model = nullptr;
      Check(*api, api->Manager_GetCatalog(manager, &catalog));
      Check(*api, api->GetCatalogApi()->GetModelVariant(catalog, argv[3], &model));
      int cached = 0;
      Check(*api, api->GetModelApi()->IsCached(model, &cached));
      if (cached) throw std::runtime_error("Download benchmark must not use an already-cached model");
      const auto download_start = Clock::now();
      Check(*api, api->GetModelApi()->Download(model, nullptr, nullptr));
      download_ms = Milliseconds(download_start);
      Check(*api, api->GetModelApi()->IsCached(model, &cached));
      if (!cached) throw std::runtime_error("Download did not produce a complete model cache");
      const char* path = nullptr;
      Check(*api, api->GetModelApi()->GetPath(model, &path));
      for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
        if (entry.is_regular_file()) download_bytes += entry.file_size();
      }
    }

    const auto destroy_start = Clock::now();
    api->Manager_Release(manager);
    const double destroy_ms = Milliseconds(destroy_start);
    std::printf(
        "FOUNDRY_BENCHMARK {\"load_ms\":%.6f,\"create_ms\":%.6f,\"destroy_ms\":%.6f,"
        "\"download_ms\":%.6f,\"download_bytes\":%llu}\n",
        load_ms, create_ms, destroy_ms, download_ms, static_cast<unsigned long long>(download_bytes));
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "SDK benchmark failed: %s\n", error.what());
    return 1;
  }
}
