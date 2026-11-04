// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <foundry_local/foundry_local_cpp.h>

#include <filesystem>
#include <iostream>

#include "samples/cpp/sample_common.h"

int main(int argc, char* argv[]) {
  using namespace foundry_local;

  if (argc != 2) {
    std::cerr << "Usage: byom <model-directory>\n";
    return 1;
  }

  try {
    Manager manager(Configuration{std::string(sample::kAppName)});
    sample::RegisterExecutionProviders(manager);
    auto& catalog = manager.GetCatalog(FOUNDRY_LOCAL_CATALOG_LOCAL);
    ModelInfo metadata;
    metadata.SetStringProperty(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR, "chat-completion");
    auto model = catalog.RegisterModel(std::filesystem::absolute(argv[1]).string(), "sample-byom:1", metadata);

    try {
      sample::LoadedModel loaded_model(*model);
      ChatSession session(*model);
      Request request{UserMessage("Why is the sky blue? Answer in two sentences.")};
      const auto response = session.ProcessRequest(request);
      std::cout << "Assistant: ";
      sample::PrintAssistantMessages(response);
    } catch (...) {
      catalog.UnregisterModel("sample-byom:1");
      throw;
    }

    catalog.UnregisterModel("sample-byom:1");
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << "\n";
    return 1;
  }

  return 0;
}
