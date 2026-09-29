// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Example: Tool calling with the Foundry Local C++ SDK.
// Demonstrates: defining tools, handling tool calls, and returning tool results
// using static factories on Item and convenience free functions (UserMessage, etc.).
// Also demonstrates streaming tool calling — the streaming callback receives both
// TextItem (visible assistant text) and ToolCallItem (one per complete tool call).

#include <foundry_local/foundry_local_cpp.h>

#include <iostream>
#include <string>

using namespace foundry_local;

// Simulated tool implementation.
static std::string get_weather(const std::string& /*location*/) {
  return R"({"temperature": 22, "unit": "celsius", "condition": "sunny"})";
}

static constexpr const char* kWeatherSchema = R"({
  "type": "object",
  "properties": {
    "location": { "type": "string", "description": "City name" }
  },
  "required": ["location"]
})";

static ToolDefinition MakeWeatherTool() {
  return ToolDefinition{
      "get_weather",
      "Get current weather for a location",
      kWeatherSchema,
  };
}

// Drive the tool-call loop until the model produces a final assistant message.
// The caller supplies the initial response; this helper handles any follow-up tool calls.
static void RunToolLoop(ChatSession& session, Response response) {
  constexpr int kMaxToolIterations = 10;
  int iteration = 0;

  while (true) {
    if (++iteration > kMaxToolIterations) {
      throw Error("Exceeded maximum tool-call iterations (" +
                      std::to_string(kMaxToolIterations) + ") without final response",
                  FOUNDRY_LOCAL_ERROR_INTERNAL);
    }

    const auto& items = response.GetItems();
    if (items.empty()) {
      throw Error("Response contained no items", FOUNDRY_LOCAL_ERROR_INTERNAL);
    }

    const auto& item = items.front();

    if (item.GetType() == FOUNDRY_LOCAL_ITEM_MESSAGE) {
      auto msg = item.GetMessage();
      std::cout << "Assistant: " << msg.GetSimpleText() << "\n";
      return;
    }

    if (item.GetType() == FOUNDRY_LOCAL_ITEM_TOOL_CALL) {
      auto [call_id, tool_name, arguments] = item.GetToolCall();
      std::cout << "Tool call: " << tool_name << "(" << arguments << ")\n";

      // Execute the tool and feed the result back. The session retains prior turns,
      // so we only need to send the new tool-result item. Clear tool_choice so the
      // model can produce a final text response.
      std::string result = get_weather("Paris");

      Request tool_request;
      tool_request.AddItem(Item::ToolResult(std::string(call_id), result));
      tool_request.SetOptions({
          .search = {.max_output_tokens = 256},
          .tool_choice = FOUNDRY_LOCAL_TOOL_CHOICE_NONE,
      });

      response = session.ProcessRequest(tool_request);
      continue;
    }

    throw Error("Unexpected item type in response", FOUNDRY_LOCAL_ERROR_INTERNAL);
  }
}

/// Non-streaming tool calling example.
/// Sends a user question, lets the model issue a tool call, returns the tool result,
/// and reads the final assistant message.
void ToolCalling(IModel& model) {
  ChatSession session(model);

  // Add the tool definition to the session so it's available across multiple ProcessRequest calls.
  session.AddToolDefinition(MakeWeatherTool());

  Request request;
  request.AddItem(UserMessage("What's the weather in Paris?"));

  // tool_choice=required forces the model to call a tool on the first turn.
  request.SetOptions({
      .search = {.max_output_tokens = 256},
      .tool_choice = FOUNDRY_LOCAL_TOOL_CHOICE_REQUIRED,
  });

  Response response = session.ProcessRequest(request);
  RunToolLoop(session, std::move(response));
}

/// Streaming tool calling example.
/// Demonstrates that the streaming callback receives BOTH TextItem (visible assistant
/// text, token-by-token) AND ToolCallItem (one per complete tool call) as the model
/// produces them. The full Response is still available after streaming completes and
/// drives the tool-call loop.
void StreamingToolCalling(IModel& model) {
  ChatSession session(model);
  session.AddToolDefinition(MakeWeatherTool());

  // The callback runs on the inference thread and receives one item per invocation.
  // Tokens that belong inside a tool-call block are buffered internally and emitted
  // as a single ToolCallItem once the block closes, so the callback never sees raw
  // `<tool_call>...</tool_call>` markers as text.
  session.SetStreamingCallback([](Item item) -> int {
    switch (item.GetType()) {
      case FOUNDRY_LOCAL_ITEM_TEXT:
        std::cout << item.GetText().text << std::flush;
        break;

      case FOUNDRY_LOCAL_ITEM_TOOL_CALL: {
        auto [call_id, tool_name, arguments] = item.GetToolCall();
        std::cout << "\n[stream] Tool call: " << tool_name << "(" << arguments << ")\n";
        break;
      }

      default:
        std::cerr << "[stream] Unexpected item type: " << static_cast<int>(item.GetType()) << "\n";
        break;
    }

    return 0;  // return non-zero to cancel
  });

  Request request;
  request.AddItem(UserMessage("What's the weather in Paris?"));
  request.SetOptions({
      .search = {.max_output_tokens = 256},
      .tool_choice = FOUNDRY_LOCAL_TOOL_CHOICE_REQUIRED,
  });

  std::cout << "Assistant: ";
  Response response = session.ProcessRequest(request);
  std::cout << "\n";

  // After streaming completes the authoritative Response is available with the same
  // tool-call ids that the callback observed. Drive the tool-call loop from here —
  // subsequent turns also stream through the same callback.
  RunToolLoop(session, std::move(response));
}

int main() {
  try {
    Configuration config("tool_calling_example");
    Manager manager(std::move(config));
    auto& catalog = manager.GetCatalog();
    auto model = catalog.GetModel("qwen2.5-0.5b");
    if (!model) {
      std::cerr << "Model not found.\n";
      return 1;
    }

    if (!model->IsCached()) {
      std::cout << "Downloading model..." << std::flush;
      model->Download([](float pct) {
        std::cout << "\rDownloading model... " << static_cast<int>(pct) << "%" << std::flush;
        return true;
      });
      std::cout << "\n";
    }

    if (!model->IsLoaded()) {
      model->Load();
    }

    std::cout << "\n--- Non-streaming tool calling ---\n";
    ToolCalling(*model);

    std::cout << "\n--- Streaming tool calling ---\n";
    StreamingToolCalling(*model);

    model->Unload();
  } catch (const Error& ex) {
    std::cerr << "Foundry Local error [" << ex.Code() << "]: " << ex.what() << "\n";
    return 1;
  }

  return 0;
}
