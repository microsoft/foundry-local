// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include <foundry_local/foundry_local_cpp.h>

#include <iostream>

int main() {
  try {
    foundry_local::Manager manager(foundry_local::Configuration("RegistryDemo"));
    manager.DownloadAndRegisterEps();
    auto& catalog = manager.GetCatalog(
        FOUNDRY_LOCAL_CATALOG_AZURE_REGISTRY,
        foundry_local::KeyValuePairs{{"name", "azureml-dev"}, {"credential", "azure_cli"}});
    for (const auto& model : catalog.GetModels()) {
      for (const auto& variant : model->GetVariants()) {
        std::cout << "- " << model->GetInfo().Alias() << " " << variant->GetInfo().Id() << "\n";
      }
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
