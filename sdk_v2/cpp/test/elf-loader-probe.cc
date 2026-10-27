// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include <foundry_local/foundry_local_c.h>
#include <curl/curl.h>
#include <dlfcn.h>

#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <string>
#include <thread>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << std::endl;
    std::exit(EXIT_FAILURE);
  }
}

void* Open(const char* path, int flags) {
  void* handle = dlopen(path, flags);
  const char* error = handle ? nullptr : dlerror();
  Require(handle != nullptr, std::string("dlopen failed: ") + (error ? error : path));
  return handle;
}

template <typename T>
T Symbol(void* handle, const char* name) {
  void* symbol = dlsym(handle, name);
  Require(symbol != nullptr, std::string("Missing symbol: ") + name);
  return reinterpret_cast<T>(symbol);
}

void CheckStatus(const flApi* api, flStatus* status) {
  if (status == nullptr) {
    return;
  }

  const auto code = api->Status_GetErrorCode(status);
  const std::string message = api->Status_GetErrorMessage(status);
  api->Status_Release(status);
  Require(code == FOUNDRY_LOCAL_OK, message);
}

size_t Write(char* data, size_t size, size_t count, void* context) {
  static_cast<std::string*>(context)->append(data, size * count);
  return size * count;
}

void CheckHostCurl(void* library, const char* url) {
  auto init = Symbol<decltype(&curl_easy_init)>(library, "curl_easy_init");
  auto setopt = Symbol<decltype(&curl_easy_setopt)>(library, "curl_easy_setopt");
  auto perform = Symbol<decltype(&curl_easy_perform)>(library, "curl_easy_perform");
  auto cleanup = Symbol<decltype(&curl_easy_cleanup)>(library, "curl_easy_cleanup");
  CURL* handle = init();
  Require(handle != nullptr, "System curl_easy_init failed");
  std::string body;
  Require(setopt(handle, CURLOPT_URL, url) == CURLE_OK, "System curl URL setup failed");
  Require(setopt(handle, CURLOPT_WRITEFUNCTION, Write) == CURLE_OK, "System curl callback setup failed");
  Require(setopt(handle, CURLOPT_WRITEDATA, &body) == CURLE_OK, "System curl data setup failed");
  Require(perform(handle) == CURLE_OK, "System curl request failed");
  cleanup(handle);
  Require(body == "host curl remains independent\n", "System curl returned an unexpected payload");
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 7, "Expected library, host curl, file URL, catalog URL, output directory and load order");
  void* curl = nullptr;
  if (std::string(argv[6]) == "curl-first") {
    curl = Open(argv[2], RTLD_NOW | RTLD_GLOBAL);
    CheckHostCurl(curl, argv[3]);
  }

  const auto directory = std::filesystem::absolute(argv[1]).parent_path();
  void* ort = Open((directory / "libonnxruntime.so").c_str(), RTLD_NOW | RTLD_LOCAL);
  void* genai = Open((directory / "libonnxruntime-genai.so").c_str(), RTLD_NOW | RTLD_LOCAL);
  void* foundry = Open(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (curl == nullptr) {
    curl = Open(argv[2], RTLD_NOW | RTLD_GLOBAL);
  }

  CheckHostCurl(curl, argv[3]);
  Require(dlsym(foundry, "curl_easy_init") == nullptr, "Foundry exposes bundled curl");
  const flApi* api = Symbol<decltype(&FoundryLocalGetApi)>(foundry, "FoundryLocalGetApi")(
      FOUNDRY_LOCAL_API_VERSION);
  Require(api != nullptr, "FoundryLocalGetApi returned null");

  std::promise<void> ready;
  auto worker_ready = ready.get_future();
  std::promise<void> exit;
  auto may_exit = exit.get_future();
  std::thread worker([&] {
    const auto* config_api = api->GetConfigurationApi();
    flConfiguration* config = nullptr;
    CheckStatus(api, config_api->Create("elf-loader-test", &config));
    CheckStatus(api, config_api->SetAppDataDir(config, argv[5]));
    CheckStatus(api, config_api->SetModelCacheDir(config, argv[5]));
    CheckStatus(api, config_api->AddCatalogUrl(config, argv[4], nullptr));
    flManager* manager = nullptr;
    CheckStatus(api, api->Manager_Create(config, &manager));
    config_api->Configuration_Release(config);

    flCatalog* catalog = nullptr;
    CheckStatus(api, api->Manager_GetCatalog(manager, &catalog));
    flModelList* models = nullptr;
    CheckStatus(api, api->GetCatalogApi()->GetModels(catalog, &models));
    // Catalog fetch failures fall back to the disk cache. The driver verifies
    // ClientHello so an empty cache alone cannot satisfy this regression.
    Require(models != nullptr && api->ModelList_Size(models) == 0, "Expected an empty catalog after TLS failure");
    api->ModelList_Release(models);

    api->Manager_Release(manager);
    ready.set_value();
    may_exit.wait();
    // OpenSSL's pthread-local destructors run only now, after the final dlclose.
  });

  worker_ready.wait();
  Require(dlclose(foundry) == 0, "Foundry dlclose failed");
  exit.set_value();
  worker.join();
  CheckHostCurl(curl, argv[3]);
  Require(dlclose(genai) == 0, "GenAI dlclose failed");
  Require(dlclose(ort) == 0, "ORT dlclose failed");
  Require(dlclose(curl) == 0, "System curl dlclose failed");
  return EXIT_SUCCESS;
}
