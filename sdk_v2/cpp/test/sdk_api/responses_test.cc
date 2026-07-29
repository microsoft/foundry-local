// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Web service integration tests for the /v1/responses endpoint.

#include "web_service_fixture.h"

#include "utils/string_utils.h"

using fl::test::ToLower;

// Find the first output item with the given "type" value, or nullptr if not found.
static const json* FindOutputByType(const json& output, const std::string& type) {
  for (auto& item : output) {
    if (item.value("type", "") == type) {
      return &item;
    }
  }
  return nullptr;
}

// Validate reasoning output item structure if present in the output array.
// Returns true if a reasoning item was found (so the caller knows the model is a reasoning model).
static bool ValidateReasoningOutput(const json& output, const char* context) {
  auto* reasoning = FindOutputByType(output, "reasoning");
  if (!reasoning) {
    return false;
  }
  EXPECT_TRUE(reasoning->contains("id")) << context << ": reasoning item missing 'id'";
  EXPECT_EQ((*reasoning)["status"], "completed") << context << ": reasoning item not completed";
  EXPECT_TRUE(reasoning->contains("summary")) << context << ": reasoning item missing 'summary'";
  if (reasoning->contains("summary")) {
    EXPECT_FALSE((*reasoning)["summary"].empty()) << context << ": reasoning summary is empty";
  }
  return true;
}

TEST_F(WebServiceIntegrationTest, ResponsesCreateAndRetrieve) {
  auto client = MakeClient();

  // Create a response with store=true
  json request_body = {
      {"model", model_id()},
      {"input", "What is 2+2? Answer with just the number."},
      {"store", true},
      {"max_output_tokens", 512},
      {"temperature", 0},
  };

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  EXPECT_TRUE(response.contains("id"));
  EXPECT_EQ(response["object"], "response");
  EXPECT_EQ(response["model"], model_id());
  EXPECT_TRUE(response.contains("output"));
  EXPECT_TRUE(response.contains("created_at"));

  EXPECT_EQ(response["status"], "completed");
  EXPECT_FALSE(response["output"].empty()) << "Expected non-empty output array";
  ValidateReasoningOutput(response["output"], "ResponsesCreateAndRetrieve");
  auto* msg_output = FindOutputByType(response["output"], "message");
  ASSERT_NE(msg_output, nullptr) << "No message output item found. Output: " << response["output"].dump();
  EXPECT_EQ((*msg_output)["role"], "assistant");
  ASSERT_TRUE(msg_output->contains("content"));
  EXPECT_FALSE((*msg_output)["content"].empty()) << "Expected non-empty content array";

  ASSERT_TRUE(response.contains("output_text"));
  EXPECT_FALSE(response["output_text"].get<std::string>().empty()) << "Expected non-empty output_text";

  ASSERT_TRUE(response.contains("usage"));
  auto& usage = response["usage"];
  EXPECT_GT(usage["input_tokens"].get<int>(), 0);
  EXPECT_GT(usage["output_tokens"].get<int>(), 0);
  int total = usage["total_tokens"].get<int>();
  EXPECT_EQ(total, usage["input_tokens"].get<int>() + usage["output_tokens"].get<int>());

  std::string response_id = response["id"].get<std::string>();

  // Retrieve the stored response
  auto get_result = client.Get(("/v1/responses/" + response_id).c_str());
  ASSERT_TRUE(get_result) << "HTTP request failed";
  ASSERT_EQ(get_result->status, 200) << get_result->body;

  json retrieved = json::parse(get_result->body);
  EXPECT_EQ(retrieved["id"], response_id);
  EXPECT_EQ(retrieved["object"], "response");
  EXPECT_EQ(retrieved["status"], "completed");
  EXPECT_FALSE(retrieved["output"].empty());

  // Get input items for the response
  auto items_result = client.Get(("/v1/responses/" + response_id + "/input_items").c_str());
  ASSERT_TRUE(items_result) << "HTTP request failed";
  ASSERT_EQ(items_result->status, 200) << items_result->body;

  json items = json::parse(items_result->body);
  EXPECT_EQ(items["object"], "list");
  EXPECT_TRUE(items.contains("data"));
  EXPECT_FALSE(items["data"].empty()) << "Expected input items";
}

TEST_F(WebServiceIntegrationTest, ResponsesCreateWithArrayInput) {
  auto client = MakeClient();
  json request_body = {
      {"model", model_id()},
      {"input", json::array({
                    {{"role", "system"}, {"content", "You are a helpful assistant. Be brief."}},
                    {{"role", "user"}, {"content", "What is the capital of France?"}},
                })},
      {"store", true},
      {"max_output_tokens", 512},
      {"temperature", 0},
  };

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  EXPECT_EQ(response["object"], "response");
  EXPECT_TRUE(response.contains("output"));
  EXPECT_FALSE(response["output"].empty());

  EXPECT_EQ(response["status"], "completed");
  EXPECT_EQ(response["model"], model_id());
  ValidateReasoningOutput(response["output"], "ResponsesCreateWithArrayInput");
  auto* msg_output = FindOutputByType(response["output"], "message");
  ASSERT_NE(msg_output, nullptr) << "No message output item found. Output: " << response["output"].dump();
  EXPECT_EQ((*msg_output)["role"], "assistant");
  ASSERT_TRUE(msg_output->contains("content"));
  EXPECT_FALSE((*msg_output)["content"].empty());

  ASSERT_TRUE(response.contains("output_text"));
  std::string output_text = response["output_text"].get<std::string>();
  EXPECT_FALSE(output_text.empty());
  EXPECT_NE(output_text.find("Paris"), std::string::npos)
      << "Expected 'Paris' in response. Got: " << output_text;
}

TEST_F(WebServiceIntegrationTest, ResponsesList) {
  auto client = MakeClient();
  auto result = client.Get("/v1/responses");
  ASSERT_TRUE(result) << "HTTP request failed";
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  EXPECT_EQ(response["object"], "list");
  EXPECT_TRUE(response.contains("data"));
  EXPECT_TRUE(response.contains("has_more"));
}

TEST_F(WebServiceIntegrationTest, ResponsesDeleteAndVerify) {
  auto client = MakeClient();

  // Create a response to delete
  json request_body = {
      {"model", model_id()},
      {"input", "Say hello."},
      {"store", true},
      {"max_output_tokens", 512},
      {"temperature", 0},
  };

  auto create_result = client.Post("/v1/responses", request_body.dump(), "application/json");
  ASSERT_TRUE(create_result) << "HTTP request failed";
  ASSERT_EQ(create_result->status, 200);

  json created = json::parse(create_result->body);
  std::string response_id = created["id"].get<std::string>();

  // Delete it
  auto delete_result = client.Delete(("/v1/responses/" + response_id).c_str());
  ASSERT_TRUE(delete_result) << "HTTP request failed";
  ASSERT_EQ(delete_result->status, 200) << delete_result->body;

  json deleted = json::parse(delete_result->body);
  EXPECT_EQ(deleted["id"], response_id);
  EXPECT_EQ(deleted["object"], "response.deleted");
  EXPECT_EQ(deleted["deleted"], true);

  // Verify it's gone
  auto get_result = client.Get(("/v1/responses/" + response_id).c_str());
  ASSERT_TRUE(get_result) << "HTTP request failed";
  EXPECT_EQ(get_result->status, 404);
}

TEST_F(WebServiceIntegrationTest, ResponsesDeleteNotFound) {
  auto client = MakeClient();
  auto result = client.Delete("/v1/responses/resp_nonexistent");
  ASSERT_TRUE(result) << "HTTP request failed";
  EXPECT_EQ(result->status, 404);
}

TEST_F(WebServiceIntegrationTest, ResponsesGetNotFound) {
  auto client = MakeClient();
  auto result = client.Get("/v1/responses/resp_nonexistent");
  ASSERT_TRUE(result) << "HTTP request failed";
  EXPECT_EQ(result->status, 404);
}

TEST_F(WebServiceIntegrationTest, ResponsesGetInputItemsNotFound) {
  auto client = MakeClient();
  auto result = client.Get("/v1/responses/resp_nonexistent/input_items");
  ASSERT_TRUE(result) << "HTTP request failed";
  EXPECT_EQ(result->status, 404);
}

TEST_F(WebServiceIntegrationTest, ResponsesEmptyBody) {
  auto client = MakeClient();
  auto result = client.Post("/v1/responses", "", "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  EXPECT_EQ(result->status, 400);
}

TEST_F(WebServiceIntegrationTest, ResponsesBadJson) {
  auto client = MakeClient();
  auto result = client.Post("/v1/responses", "{not json", "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  EXPECT_EQ(result->status, 400);
}

TEST_F(WebServiceIntegrationTest, ResponsesMissingModel) {
  auto client = MakeClient();
  json request_body = {
      {"input", "Hello"},
  };

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  EXPECT_EQ(result->status, 400);
}

TEST_F(WebServiceIntegrationTest, ResponsesMissingInput) {
  auto client = MakeClient();
  json request_body = {
      {"model", model_id()},
  };

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  EXPECT_EQ(result->status, 400);
}

TEST_F(WebServiceIntegrationTest, ResponsesModelNotFound) {
  auto client = MakeClient();
  json request_body = {
      {"model", "nonexistent_model_xyz"},
      {"input", "Hello"},
  };

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  EXPECT_EQ(result->status, 404);
}

TEST_F(WebServiceIntegrationTest, ResponsesStoreDisabled) {
  auto client = MakeClient();
  json request_body = {
      {"model", model_id()},
      {"input", "Say hello."},
      {"store", false},
      {"max_output_tokens", 512},
      {"temperature", 0},
  };

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  EXPECT_EQ(response["status"], "completed");
  std::string response_id = response["id"].get<std::string>();

  auto get_result = client.Get(("/v1/responses/" + response_id).c_str());
  ASSERT_TRUE(get_result) << "HTTP request failed";
  EXPECT_EQ(get_result->status, 404) << "store=false response should not be retrievable";
}

TEST_F(WebServiceIntegrationTest, ResponsesPreviousResponseId) {
  auto client = MakeClient();

  json first_request = {
      {"model", model_id()},
      {"input", "Remember: the secret word is 'banana'."},
      {"store", true},
      {"max_output_tokens", 1024},
      {"temperature", 0},
  };

  auto first_result = client.Post("/v1/responses", first_request.dump(), "application/json");
  ASSERT_TRUE(first_result) << "HTTP request failed";
  ASSERT_EQ(first_result->status, 200) << first_result->body;

  json first_response = json::parse(first_result->body);
  std::string first_id = first_response["id"].get<std::string>();
  ASSERT_EQ(first_response["status"], "completed")
      << "Turn 1 must complete (not 'incomplete' from max_output_tokens) before chaining. Body: "
      << first_result->body;

  // Guard: confirm the magic word landed in Turn 1's output. If it didn't,
  // Turn 2 can't possibly recall it and would fail with a misleading
  // "expected 'banana' in response" error pointing at the wrong turn.
  ASSERT_TRUE(first_response.contains("output_text"));
  std::string first_output_text = first_response["output_text"].get<std::string>();
  ASSERT_NE(first_output_text.find("banana"), std::string::npos)
      << "Turn 1 did not echo the secret word. Got: " << first_output_text;

  json second_request = {
      {"model", model_id()},
      {"input", "What is the secret word?"},
      {"previous_response_id", first_id},
      {"store", true},
      {"max_output_tokens", 1024},
      {"temperature", 0},
  };

  auto second_result = client.Post("/v1/responses", second_request.dump(), "application/json");
  ASSERT_TRUE(second_result) << "HTTP request failed";
  ASSERT_EQ(second_result->status, 200) << second_result->body;

  json second_response = json::parse(second_result->body);
  ASSERT_EQ(second_response["status"], "completed") << second_result->body;
  EXPECT_FALSE(second_response["output"].empty());

  ValidateReasoningOutput(second_response["output"], "ResponsesPreviousResponseId");
  auto* msg_output = FindOutputByType(second_response["output"], "message");
  ASSERT_NE(msg_output, nullptr) << "No message output item found. Output: " << second_response["output"].dump();
  EXPECT_EQ((*msg_output)["role"], "assistant");

  ASSERT_TRUE(second_response.contains("output_text"));
  std::string output_text = second_response["output_text"].get<std::string>();
  EXPECT_NE(output_text.find("banana"), std::string::npos)
      << "Expected 'banana' in response. Got: " << output_text;
}

TEST_F(WebServiceIntegrationTest, ResponsesCreateStreaming) {
  auto client = MakeClient();
  json request_body = {
      {"model", model_id()},
      {"input", "Say the word 'hello'."},
      {"stream", true},
      {"max_output_tokens", 512},
      {"temperature", 0},
  };

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;

  std::string body = result->body;
  int event_count = 0;
  std::string assembled_text;
  bool got_completed = false;
  std::vector<std::string> event_types;

  std::istringstream stream(body);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (line.rfind("data: ", 0) == 0) {
      std::string data = line.substr(6);
      if (data == "[DONE]") {
        break;
      }

      json event = json::parse(data);
      ASSERT_TRUE(event.contains("type")) << "SSE event missing 'type': " << data;
      std::string event_type = event["type"].get<std::string>();
      event_types.push_back(event_type);

      if (event_type == "response.completed") {
        got_completed = true;
        if (event.contains("response")) {
          auto& resp = event["response"];
          EXPECT_EQ(resp["status"], "completed");
          EXPECT_FALSE(resp["output"].empty());
        }
      }

      if (event_type == "response.output_text.delta") {
        if (event.contains("delta")) {
          assembled_text += event["delta"].get<std::string>();
        }
      }

      ++event_count;
    }
  }

  EXPECT_TRUE(got_completed) << "Stream should contain response.completed event";
  EXPECT_GT(event_count, 0) << "Should have received at least one SSE event";
  EXPECT_FALSE(assembled_text.empty()) << "Assembled streaming text should not be empty";
  std::string lower_text = ToLower(assembled_text);
  EXPECT_NE(lower_text.find("hello"), std::string::npos)
      << "Expected 'hello' (case-insensitive) in streaming output. Got: " << assembled_text;

  ASSERT_FALSE(event_types.empty());
  EXPECT_EQ(event_types.front(), "response.created") << "First event should be response.created";

  std::cout << "Streaming responses output (" << event_count << " events): " << assembled_text << "\n";
}

TEST_F(WebServiceIntegrationTest, ResponsesStreamingThenChainNonStreaming) {
  auto client = MakeClient();

  // Turn 1: streaming with store=true — session gets CheckIn'd from the background thread
  json first_request = {
      {"model", model_id()},
      {"input", "Remember: the password is 'mango'."},
      {"stream", true},
      {"store", true},
      {"max_output_tokens", 512},
      {"temperature", 0},
  };

  auto first_result = client.Post("/v1/responses", first_request.dump(), "application/json");
  ASSERT_TRUE(first_result) << "HTTP request failed";
  ASSERT_EQ(first_result->status, 200) << first_result->body;

  // Parse streaming response to find the response ID from response.completed event.
  // Capture the inner status so we can fail loudly here (rather than in Turn 2's content check)
  // if Turn 1 ran out of token budget. We do NOT assert Turn 1 content — with the small test model,
  // a polite acknowledgement ("Got it!") that doesn't echo the password is normal and harmless,
  // because the password is in the input which is committed to the session history regardless.
  // The real chain validation is on Turn 2.
  std::string first_id;
  std::string first_status;
  std::istringstream stream(first_result->body);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (line.rfind("data: ", 0) == 0) {
      std::string data = line.substr(6);
      if (data == "[DONE]") {
        break;
      }

      json event = json::parse(data);
      std::string event_type = event.value("type", "");

      if (event_type == "response.completed" && event.contains("response")) {
        first_id = event["response"]["id"].get<std::string>();
        first_status = event["response"].value("status", "");
      }
    }
  }

  ASSERT_FALSE(first_id.empty()) << "Should have received response.completed with an ID";
  ASSERT_EQ(first_status, "completed")
      << "Turn 1 streaming response did not complete cleanly (likely 'incomplete' from max_output_tokens).";

  // Turn 2: non-streaming, chaining from the streaming response
  json second_request = {
      {"model", model_id()},
      {"input", "What is the password?"},
      {"previous_response_id", first_id},
      {"store", true},
      {"max_output_tokens", 512},
      {"temperature", 0},
  };

  auto second_result = client.Post("/v1/responses", second_request.dump(), "application/json");
  ASSERT_TRUE(second_result) << "HTTP request failed";
  ASSERT_EQ(second_result->status, 200) << second_result->body;

  json second_response = json::parse(second_result->body);
  ASSERT_EQ(second_response["status"], "completed") << second_result->body;

  std::string output_text = second_response["output_text"].get<std::string>();
  EXPECT_NE(output_text.find("mango"), std::string::npos)
      << "Expected 'mango' in chained response. Got: " << output_text;
}

TEST_F(WebServiceIntegrationTest, ResponsesNonStreamingThenChainStreaming) {
  auto client = MakeClient();

  // Turn 1: non-streaming with store=true
  json first_request = {
      {"model", model_id()},
      {"input", "Remember: the code word is 'cherry'."},
      {"store", true},
      {"max_output_tokens", 512},
      {"temperature", 0},
  };

  auto first_result = client.Post("/v1/responses", first_request.dump(), "application/json");
  ASSERT_TRUE(first_result) << "HTTP request failed";
  ASSERT_EQ(first_result->status, 200) << first_result->body;

  json first_response = json::parse(first_result->body);
  std::string first_id = first_response["id"].get<std::string>();
  ASSERT_EQ(first_response["status"], "completed")
      << "Turn 1 must complete (not 'incomplete' from max_output_tokens) before chaining. Body: "
      << first_result->body;

  // We do NOT assert Turn 1 content. The small test model often replies with a polite
  // acknowledgement ("Got it!") that doesn't echo the code word, which is fine — the input
  // itself is committed to session history regardless. The real chain validation is on Turn 2.

  // Turn 2: streaming, chaining from the non-streaming response
  json second_request = {
      {"model", model_id()},
      {"input", "What is the code word?"},
      {"previous_response_id", first_id},
      {"stream", true},
      {"store", true},
      {"max_output_tokens", 512},
      {"temperature", 0},
  };

  auto second_result = client.Post("/v1/responses", second_request.dump(), "application/json");
  ASSERT_TRUE(second_result) << "HTTP request failed";
  ASSERT_EQ(second_result->status, 200) << second_result->body;

  std::string assembled_text;
  std::string second_status;
  bool got_completed = false;

  std::istringstream stream2(second_result->body);
  std::string line2;
  while (std::getline(stream2, line2)) {
    if (!line2.empty() && line2.back() == '\r') {
      line2.pop_back();
    }

    if (line2.rfind("data: ", 0) == 0) {
      std::string data = line2.substr(6);
      if (data == "[DONE]") {
        break;
      }

      json event = json::parse(data);
      std::string event_type = event.value("type", "");

      if (event_type == "response.output_text.delta" && event.contains("delta")) {
        assembled_text += event["delta"].get<std::string>();
      }

      if (event_type == "response.completed") {
        got_completed = true;
        if (event.contains("response")) {
          second_status = event["response"].value("status", "");
        }
      }
    }
  }

  ASSERT_TRUE(got_completed) << "Stream should contain response.completed event";
  ASSERT_EQ(second_status, "completed")
      << "Turn 2 streaming response did not complete cleanly (likely 'incomplete' from max_output_tokens).";
  EXPECT_FALSE(assembled_text.empty()) << "Assembled streaming text should not be empty";
  EXPECT_NE(assembled_text.find("cherry"), std::string::npos)
      << "Expected 'cherry' in streaming chained response. Got: " << assembled_text;
}
