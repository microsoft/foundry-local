// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Example: Basic chat with the Foundry Local C++ SDK.
// Demonstrates: create manager, browse catalog, download and load a model,
// create a chat session, and run inference with the type-safe item API.
// Also demonstrates streaming inference with a callback.

#include <foundry_local/foundry_local_cpp.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

using namespace foundry_local;

/// Non-streaming chat example.
/// Creates a session, sends a request, reads the full response, and does a follow-up turn.
void BasicChat(IModel& model) {
  // 1. Create a session for multi-turn chat.
  ChatSession session(model);

  // 2. Build a request with type-specific item classes.
  Request request{
      // optional to set system message.
      // SystemMessage("You are a helpful assistant."),
      UserMessage("What is the capital of France?"),
  };

  // 3. Process request.
  // What this equates to and the allowed input types is determined by the session/model type
  // e.g. chat model requires messages as input, realtime audio might take an audio file or a realtime stream,
  //      predictive model takes tensors, embeddings model takes text.
  Response response = session.ProcessRequest(request);

  // 4. Read the response — structured bindings on content structs.
  for (const auto& item : response.GetItems()) {
    switch (item.GetType()) {
      case FOUNDRY_LOCAL_ITEM_MESSAGE: {
        auto msg = item.GetMessage();
        std::cout << "Assistant: " << msg.GetSimpleText() << "\n";
        break;
      }
      case FOUNDRY_LOCAL_ITEM_TOOL_CALL: {
        auto [call_id, name, arguments] = item.GetToolCall();
        std::cout << "Tool call: " << name << "(" << arguments << ")\n";
        break;
      }
      default:
        break;
    }
  }

  // 5. Check finish reason and usage.
  if (response.GetFinishReason() == FOUNDRY_LOCAL_FINISH_STOP) {
    flUsage usage = response.GetUsage();
    std::cout << "Tokens — prompt: " << usage.prompt_tokens
              << ", completion: " << usage.completion_tokens
              << ", total: " << usage.total_tokens << "\n";
  }

  // 6. Continue the conversation — add a follow-up turn.
  // The session has the context of the previous turn, so we only need to add the new user message in this request.
  Request follow_up;
  follow_up.AddItem(UserMessage("And what is its population?"));

  Response follow_up_response = session.ProcessRequest(follow_up);

  if (!follow_up_response.GetItems().empty()) {
    const auto& front = follow_up_response.GetItems().front();
    if (front.GetType() == FOUNDRY_LOCAL_ITEM_MESSAGE) {
      auto msg = front.GetMessage();
      std::cout << "Assistant: " << msg.GetSimpleText() << "\n";
    } else {
      std::cerr << "Unexpected item type in follow-up response: "
                << static_cast<int>(front.GetType()) << "\n";
    }
  }
}

/// Streaming chat example.
/// Sets a streaming callback on the session so incremental tokens are printed as they arrive.
void StreamingChat(IModel& model) {
  // 1. Create a session for multi-turn chat.
  ChatSession session(model);

  // 2. Set a streaming callback. Each invocation receives one item in the queue.
  //    We pop the item from the queue and print any message content incrementally.

  // lambda can capture user data if needed (equivalent to the additional `void* user_data` parameter in the C API).
  session.SetStreamingCallback([/*user_data*/](flStreamingCallbackData event) -> int {
    const auto* item_api = detail::item_api();

    flItem* raw_item = nullptr;
    if (item_api->ItemQueue_TryPop(event.item_queue, &raw_item)) {
      Item item(*raw_item);
      // in this example we're streaming each token generated as simple text.
      // what is streamed is flexible though given flItem is being used
      if (item.GetType() == FOUNDRY_LOCAL_ITEM_TEXT) {
        std::cout << item.GetText().text << std::flush;
      } else {
        std::cerr << "Unexpected item type" << std::endl;
      }

      // ~Item calls release.
    } else {
      // should never happen. adding item to queue to callback should be 1:1.
      std::cerr << "Callback invoked but no item in queue" << std::endl;
    }

    return 0;  // return non-zero to cancel
  });

  // 3. Build and send the request.
  Request request{UserMessage("What is the capital of France?")};

  std::cout << "Assistant: ";
  // the callback is invoked on a separate thread as each token is produced.
  Response response = session.ProcessRequest(request);
  std::cout << "\n";

  // 4. The full response is still available after streaming completes.
  if (response.GetFinishReason() == FOUNDRY_LOCAL_FINISH_STOP) {
    flUsage usage = response.GetUsage();
    std::cout << "Tokens — prompt: " << usage.prompt_tokens
              << ", completion: " << usage.completion_tokens
              << ", total: " << usage.total_tokens << "\n";
  }
}

int main() {
  try {
    // 1. Create a configuration and manager. Manager should be a long-lived object in your app and must remain valid
    // while using the FL SDK.
    Configuration config("basic_chat");
    if (const char* cache_dir = std::getenv("FOUNDRY_LOCAL_SAMPLE_CACHE_DIR")) {
      config.SetModelCacheDir(cache_dir);
    }
    Manager manager(std::move(config));

    // 2. Get the catalog and list all available models.
    auto& catalog = manager.GetCatalog();
    ModelList all_models = catalog.GetModels();
    std::cout << "Available models: " << all_models.Models().size() << "\n";
    for (const auto& m : all_models.Models()) {
      std::cout << "  Alias:" << m->GetInfo().Alias() << "\n";
      ModelList variants = m->GetVariants();
      for (const auto& variant : variants.Models()) {
        ModelInfo info = variant->GetInfo();
        std::cout << "    " << info.Id() << (variant->IsCached() ? " [cached]" : "") << "\n";
      }
    }

    // 3. Look up a specific model by alias. This will return the default selected variant.
    //    Use GetModel(alias) to get a Model that contains all variants for an alias (and GetVariants/SelectVariant to choose),
    //    or GetModelVariant(id) to get a specific variant by ID.
    std::unique_ptr<IModel> model;
    for (const std::string alias : {"qwen2.5-0.5b", "qwen3.5-0.8b"}) {
      auto candidate = catalog.GetModel(alias);
      if (!candidate) {
        continue;
      }

      ModelList variants = candidate->GetVariants();
      for (const auto& variant : variants.Models()) {
        ModelInfo variant_info = variant->GetInfo();
        if (variant_info.DeviceType() == FOUNDRY_LOCAL_DEVICE_CPU &&
            (variant_info.Task() == "chat-completion" || variant_info.Task() == "vision-language-chat")) {
          candidate->SelectVariant(*variant);
          model = std::move(candidate);
          break;
        }
      }
      if (model) {
        break;
      }
    }
    if (!model) {
      std::cerr << "No supported CPU chat model found.\n";
      return 1;
    }

    ModelInfo info = model->GetInfo();
    std::cout << "\nUsing model: " << info.Name() << "\n";

    // 4. Download if not already cached (with cancellable progress).
    if (!model->IsCached()) {
      std::cout << "Downloading...\n";
      model->Download([](float progress) -> int {
        std::cout << "\r  " << static_cast<int>(progress) << "%" << std::flush;
        return 0;  // return non-zero to cancel
      });
      std::cout << "\n";
    }

    std::cout << "Model path: " << model->GetPath() << "\n";

    // 5. Load the model into memory.
    if (!model->IsLoaded()) {
      model->Load();
    }

    // 6. Run the non-streaming example.
    std::cout << "\n--- Non-streaming chat ---\n";
    BasicChat(*model);

    // 7. Run the streaming example.
    std::cout << "\n--- Streaming chat ---\n";
    StreamingChat(*model);

    // 8. Unload when done. Model dtor will also handle this.
    model->Unload();
  } catch (const Error& ex) {
    std::cerr << "Foundry Local error [" << ex.Code() << "]: " << ex.what()
              << "\n";
    return 1;
  }

  return 0;
}
