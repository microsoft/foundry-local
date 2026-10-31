// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <foundry_local/foundry_local_cpp.h>

#include <iostream>
#include <string>

#include "samples/cpp/sample_common.h"

namespace {

foundry_local::ToolDefinition WeatherTool() {
  return {
      "get_weather",
      "Get the current weather for a city.",
      R"({"type":"object","properties":{"city":{"type":"string"}},"required":["city"],"additionalProperties":false})",
  };
}

std::unique_ptr<foundry_local::IModel> FindToolCallingModel(foundry_local::ICatalog& catalog) {
  auto model = sample::GetModel(catalog, "qwen2.5-0.5b");
  if (!model->GetInfo().SupportsToolCalling().value_or(false)) {
    throw std::runtime_error("The selected qwen2.5-0.5b variant does not support tool calling");
  }

  return model;
}

void RunToolConversation(foundry_local::IModel& model) {
  using namespace foundry_local;

  ChatSession session(model);
  session.AddToolDefinition(WeatherTool());

  Request request{UserMessage("What is the weather in Paris?")};
  RequestOptions request_options;
  request_options.search.temperature = 0.0F;
  request_options.search.max_output_tokens = 256;
  request_options.tool_choice = FOUNDRY_LOCAL_TOOL_CHOICE_REQUIRED;
  request.SetOptions(request_options);

  auto response = session.ProcessRequest(request);
  for (const auto& item : response.GetItems()) {
    if (item.GetType() != FOUNDRY_LOCAL_ITEM_TOOL_CALL) {
      continue;
    }

    const auto call = item.GetToolCall();
    std::cout << "Tool call: " << call.name << "(" << call.arguments << ")\n";

    Request result_request{Item::ToolResult(
        std::string(call.call_id), R"({"temperature":22,"unit":"celsius","condition":"sunny"})")};
    RequestOptions result_options;
    result_options.search.temperature = 0.0F;
    result_options.search.max_output_tokens = 256;
    result_options.tool_choice = FOUNDRY_LOCAL_TOOL_CHOICE_NONE;
    result_request.SetOptions(result_options);
    const auto final_response = session.ProcessRequest(result_request);
    std::cout << "Assistant: ";
    sample::PrintAssistantMessages(final_response);
    return;
  }

  throw std::runtime_error("The model did not produce a tool call");
}

}  // namespace

int main() {
  using namespace foundry_local;

  try {
    Configuration configuration{std::string(sample::kAppName)};
    Manager manager(std::move(configuration));
    sample::RegisterExecutionProviders(manager);

    auto model = FindToolCallingModel(manager.GetCatalog());
    std::cout << "Using " << model->GetInfo().Id() << "\n";
    sample::LoadedModel loaded_model(*model);
    RunToolConversation(*model);
  } catch (const Error& error) {
    std::cerr << "Foundry Local error [" << error.Code() << "]: " << error.what() << "\n";
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << "\n";
    return 1;
  }

  return 0;
}
