// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <foundry_local/foundry_local_cpp.h>

#include <filesystem>
#include <iostream>
#include <vector>

#include "samples/cpp/sample_common.h"

int main(int argc, char* argv[]) {
  using namespace foundry_local;

  if (argc != 3) {
    std::cerr << "Usage: vision <model-alias> <image-path>\n";
    return 1;
  }

  try {
    Manager manager(Configuration{std::string(sample::kAppName)});
    sample::RegisterExecutionProviders(manager);
    auto model = sample::GetModel(manager.GetCatalog(), argv[1]);
    sample::LoadedModel loaded_model(*model);
    ChatSession session(*model);

    std::vector<Item> parts;
    parts.push_back(Item::Text("Describe this image in one short sentence."));
    parts.push_back(Item::ImageFromUri(std::filesystem::absolute(argv[2]).string()));
    Request request{MessageItem(FOUNDRY_LOCAL_ROLE_USER, std::move(parts))};

    const auto response = session.ProcessRequest(request);
    std::cout << "Assistant: ";
    sample::PrintAssistantMessages(response);
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << "\n";
    return 1;
  }

  return 0;
}
