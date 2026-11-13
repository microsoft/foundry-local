// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include <foundry_local/foundry_local_cpp.h>

#include <iostream>

using namespace foundry_local;

int main() {
  try {
    Manager manager(Configuration("RegistryDemo"));
    manager.DownloadAndRegisterEps();
    auto& catalog = manager.GetCatalog(
        FOUNDRY_LOCAL_CATALOG_AZURE_REGISTRY,
        KeyValuePairs{{"name", "azureml-dev"}, {"credential", "azure_cli"}});
    auto model = catalog.GetModel("qwen2.5-0.5b");
    if (!model) throw std::runtime_error("qwen2.5-0.5b is not available in this registry");
    std::cout << "Using " << model->GetInfo().Id() << "\n";
    model->Download();
    model->Load();
    {
      ChatSession session(*model);
      Request request{UserMessage("What is 2 + 2? Answer briefly.")};
      const auto response = session.ProcessRequest(request);
      for (const auto& item : response.GetItems()) {
        if (item.GetType() != FOUNDRY_LOCAL_ITEM_MESSAGE) continue;
        for (const auto& part : item.GetMessage().parts) {
          if (part.GetType() == FOUNDRY_LOCAL_ITEM_TEXT &&
              part.GetText().type == FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT) {
            std::cout << part.GetText().text;
          }
        }
      }
      std::cout << "\n";
    }
    model->Unload();
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
