// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include <foundry_local/foundry_local_cpp.h>

#include <iostream>

struct flConfiguration {};
struct flKeyValuePairs {};

namespace {

flConfiguration configuration;
flKeyValuePairs options;
int configurations_created = 0;
int configurations_released = 0;
int options_applied = 0;

flStatus* FL_API_CALL CreateConfiguration(const char*, flConfiguration** output) noexcept {
  ++configurations_created;
  *output = &configuration;
  return nullptr;
}

void FL_API_CALL ReleaseConfiguration(flConfiguration*) noexcept { ++configurations_released; }

flStatus* FL_API_CALL SetOptions(flConfiguration*, const flKeyValuePairs*) noexcept {
  ++options_applied;
  return nullptr;
}

const flConfigurationApi* FL_API_CALL ConfigurationApi() noexcept {
  static const flConfigurationApi api = [] {
    flConfigurationApi table{};
    table.Create = CreateConfiguration;
    table.Configuration_Release = ReleaseConfiguration;
    table.SetAdditionalOptions = SetOptions;
    return table;
  }();
  return &api;
}

void FL_API_CALL CreateOptions(flKeyValuePairs** output) noexcept { *output = &options; }

void FL_API_CALL AddOption(flKeyValuePairs*, const char*, const char*) noexcept {}
void FL_API_CALL ReleaseOptions(flKeyValuePairs*) noexcept {}

}  // namespace

extern "C" const flApi* FL_API_CALL FoundryLocalGetApi(uint32_t version) noexcept {
  static const flApi api = [] {
    flApi table{};
    table.GetConfigurationApi = ConfigurationApi;
    table.CreateKeyValuePairs = CreateOptions;
    table.AddKeyValuePair = AddOption;
    table.KeyValuePairs_Release = ReleaseOptions;
    return table;
  }();
  return version <= 2 ? &api : nullptr;
}

extern "C" const char* FL_API_CALL FoundryLocalGetVersionString() noexcept { return "v2-test-runtime"; }

int main() {
  try {
    if (foundry_local::detail::api_version() != 2 || !foundry_local::detail::api()) {
      throw std::runtime_error("C++ wrapper did not negotiate the available v2 API");
    }

    {
      foundry_local::Configuration config("v2-client");
      config.SetUseSharedHost(false);
      bool rejected = false;
      try {
        config.SetUseSharedHost(true);
      } catch (const foundry_local::Error& error) {
        rejected = error.Code() == FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED;
      }

      if (!rejected || configurations_created != 1 || options_applied != 1) {
        throw std::runtime_error("v2 configuration failed or the v3-only setting did not fail explicitly");
      }
    }

    if (configurations_released != 1) {
      throw std::runtime_error("v2 configuration was not released");
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
