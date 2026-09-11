// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Web service integration tests for the /v1/responses endpoint.

#include "web_service_fixture.h"

#include "utils/string_utils.h"

using fl::test::ToLower;

class ResponsesCrossModelIntegrationTest : public WebServiceFixture {
 protected:
  static void SetUpTestSuite() {
    SharedTestEnv::Get().AcquireModels({SharedTestEnv::Modality::Chat, SharedTestEnv::Modality::Reasoning});
  }

  void SetUp() override {
    if (!SharedTestEnv::Get().chat_model() || !SharedTestEnv::Get().reasoning_model()) {
      GTEST_SKIP() << "Two distinct loaded chat models are required";
    }
  }
};

// Find the first output item with the given "type" value, or nullptr if not found.
static const json* FindOutputByType(const json& output, const std::string& type) {
  for (auto& item : output) {
    if (item.value("type", "") == type) {
      return &item;
    }
  }
  return nullptr;
}

static std::string MessageOutputText(const json& response) {
  const auto* message = FindOutputByType(response.at("output"), "message");
  if (message == nullptr) {
    return {};
  }

  std::string text;
  for (const auto& content : message->at("content")) {
    if (content.value("type", "") == "output_text") {
      text += content.value("text", "");
    }
  }

  return text;
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

TEST_F(WebServiceIntegrationTest, ResponsesRejectsNonzeroPenaltyBeforeInference) {
  auto client = MakeClient();
  json request_body = {
      {"model", model_id()},
      {"input", "Hello"},
      {"presence_penalty", -0.3},
  };

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");

  ASSERT_TRUE(result) << "HTTP request failed";
  EXPECT_EQ(result->status, 400);
  EXPECT_EQ(json::parse(result->body)["error"]["type"], "invalid_request_error");
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
  ASSERT_TRUE(first_response.contains("previous_response_id"));
  EXPECT_TRUE(first_response["previous_response_id"].is_null());

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
  ASSERT_TRUE(second_response.contains("id"));
  std::string second_id = second_response["id"].get<std::string>();
  EXPECT_NE(second_id, first_id);
  ASSERT_TRUE(second_response.contains("previous_response_id"));
  EXPECT_EQ(second_response["previous_response_id"], first_id);
  EXPECT_FALSE(second_response["output"].empty());

  ValidateReasoningOutput(second_response["output"], "ResponsesPreviousResponseId");
  auto* msg_output = FindOutputByType(second_response["output"], "message");
  ASSERT_NE(msg_output, nullptr) << "No message output item found. Output: " << second_response["output"].dump();
  EXPECT_EQ((*msg_output)["role"], "assistant");

  auto get_result = client.Get(("/v1/responses/" + second_id).c_str());
  ASSERT_TRUE(get_result) << "HTTP request failed";
  ASSERT_EQ(get_result->status, 200) << get_result->body;

  json retrieved = json::parse(get_result->body);
  EXPECT_EQ(retrieved["id"], second_id);
  EXPECT_EQ(retrieved["previous_response_id"], first_id);
}

TEST_F(ResponsesCrossModelIntegrationTest, ContinuationRejectsADifferentResolvedModel) {
  const std::string& first_model_id = SharedTestEnv::Get().chat_model_id();
  const std::string& other_model_id = SharedTestEnv::Get().reasoning_model_id();
  if (other_model_id.empty() || other_model_id == first_model_id) {
    GTEST_SKIP() << "No distinct loaded model is available";
  }

  auto client = MakeClient();
  json first_request = {
      {"model", first_model_id},
      {"input", "Remember the word amber."},
      {"store", true},
      {"max_output_tokens", 128},
      {"temperature", 0},
  };

  auto first_result = client.Post("/v1/responses", first_request.dump(), "application/json");
  ASSERT_TRUE(first_result);
  ASSERT_EQ(first_result->status, 200) << first_result->body;
  const std::string first_id = json::parse(first_result->body)["id"].get<std::string>();

  json second_request = {
      {"model", other_model_id},
      {"input", "Continue."},
      {"previous_response_id", first_id},
      {"store", true},
  };

  auto second_result = client.Post("/v1/responses", second_request.dump(), "application/json");
  ASSERT_TRUE(second_result);
  EXPECT_EQ(second_result->status, 400) << second_result->body;

  const auto error = json::parse(second_result->body);
  EXPECT_NE(ToLower(error["error"].value("message", "")).find("different model"), std::string::npos);
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
  json completed_response;
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
          completed_response = resp;
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
  ASSERT_FALSE(completed_response.is_null());
  EXPECT_EQ(assembled_text, completed_response["output_text"].get<std::string>());
  EXPECT_EQ(assembled_text, MessageOutputText(completed_response));
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

  // Capture the complete response from response.completed so the streaming turn can be validated
  // using the same response contract as a non-streaming turn.
  json first_response;
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
        first_response = event["response"];
      }
    }
  }

  ASSERT_FALSE(first_response.is_null()) << "Should have received response.completed with response data";
  ASSERT_TRUE(first_response.contains("id"));
  std::string first_id = first_response["id"].get<std::string>();
  ASSERT_FALSE(first_id.empty()) << "Should have received response.completed with an ID";
  ASSERT_EQ(first_response["status"], "completed")
      << "Turn 1 streaming response did not complete cleanly (likely 'incomplete' from max_output_tokens).";
  ASSERT_TRUE(first_response.contains("previous_response_id"));
  EXPECT_TRUE(first_response["previous_response_id"].is_null());
  EXPECT_FALSE(first_response["output"].empty());

  ValidateReasoningOutput(first_response["output"], "ResponsesStreamingThenChainNonStreaming turn 1");
  auto* first_message = FindOutputByType(first_response["output"], "message");
  ASSERT_NE(first_message, nullptr) << "No message output item found. Output: " << first_response["output"].dump();
  EXPECT_EQ((*first_message)["role"], "assistant");
  ASSERT_TRUE(first_message->contains("content"));
  EXPECT_FALSE((*first_message)["content"].empty()) << "Expected non-empty assistant content";

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
  ASSERT_TRUE(second_response.contains("id"));
  std::string second_id = second_response["id"].get<std::string>();
  EXPECT_NE(second_id, first_id);
  ASSERT_TRUE(second_response.contains("previous_response_id"));
  EXPECT_EQ(second_response["previous_response_id"], first_id);
  EXPECT_FALSE(second_response["output"].empty());

  ValidateReasoningOutput(second_response["output"], "ResponsesStreamingThenChainNonStreaming turn 2");
  auto* second_message = FindOutputByType(second_response["output"], "message");
  ASSERT_NE(second_message, nullptr) << "No message output item found. Output: " << second_response["output"].dump();
  EXPECT_EQ((*second_message)["role"], "assistant");
  ASSERT_TRUE(second_message->contains("content"));
  EXPECT_FALSE((*second_message)["content"].empty()) << "Expected non-empty assistant content";
  ASSERT_TRUE(second_response.contains("output_text"));
  EXPECT_FALSE(second_response["output_text"].get<std::string>().empty()) << "Expected non-empty output_text";

  auto get_result = client.Get(("/v1/responses/" + second_id).c_str());
  ASSERT_TRUE(get_result) << "HTTP request failed";
  ASSERT_EQ(get_result->status, 200) << get_result->body;

  json retrieved = json::parse(get_result->body);
  EXPECT_EQ(retrieved["id"], second_id);
  EXPECT_EQ(retrieved["previous_response_id"], first_id);
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
  json completed_response;

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
          completed_response = event["response"];
          second_status = completed_response.value("status", "");
        }
      }
    }
  }

  ASSERT_TRUE(got_completed) << "Stream should contain response.completed event";
  ASSERT_EQ(second_status, "completed")
      << "Turn 2 streaming response did not complete cleanly (likely 'incomplete' from max_output_tokens).";
  EXPECT_FALSE(assembled_text.empty()) << "Assembled streaming text should not be empty";
  ASSERT_FALSE(completed_response.is_null());
  EXPECT_EQ(assembled_text, completed_response["output_text"].get<std::string>());
  EXPECT_EQ(assembled_text, MessageOutputText(completed_response));
  EXPECT_NE(assembled_text.find("cherry"), std::string::npos)
      << "Expected 'cherry' in streaming chained response. Got: " << assembled_text;
}

// ----------------------------------------------------------------------
// Cold-path replay: branching twice from the same previous_response_id.
//
// SessionManager::CheckOut removes the cached session, so the first
// continuation consumes it and the second must rebuild the conversation from
// the ResponseStore. That makes the cold path deterministic without touching
// cache internals or waiting for an eviction.
// ----------------------------------------------------------------------

TEST_F(WebServiceIntegrationTest, ResponsesBranchingTwiceForcesChainRebuild) {
  auto client = MakeClient();

  json root_request = {
      {"model", model_id()},
      {"input", "Remember: the secret word is 'banana'. Reply with just 'ok'."},
      {"store", true},
      {"max_output_tokens", 1024},
      {"temperature", 0},
  };

  auto root_result = client.Post("/v1/responses", root_request.dump(), "application/json");
  ASSERT_TRUE(root_result) << "HTTP request failed";
  ASSERT_EQ(root_result->status, 200) << root_result->body;

  json root_response = json::parse(root_result->body);
  ASSERT_EQ(root_response["status"], "completed") << root_result->body;
  const std::string root_id = root_response["id"].get<std::string>();

  auto ask_secret = [&](const char* label) {
    json branch = {
        {"model", model_id()},
        {"input", "What is the secret word? Reply with just the word."},
        {"previous_response_id", root_id},
        {"store", true},
        {"max_output_tokens", 1024},
        {"temperature", 0},
    };

    auto result = client.Post("/v1/responses", branch.dump(), "application/json");
    EXPECT_TRUE(result) << label << ": HTTP request failed";
    return result;
  };

  // First branch: the session cached under the root id is still there, so this is the warm path. Checking it out
  // is what leaves the cache empty for the second branch.
  auto warm_result = ask_secret("warm branch");
  ASSERT_TRUE(warm_result);
  ASSERT_EQ(warm_result->status, 200) << warm_result->body;
  json warm_response = json::parse(warm_result->body);
  ASSERT_EQ(warm_response["status"], "completed") << warm_result->body;
  EXPECT_EQ(warm_response["previous_response_id"], root_id);

  // Second branch from the same root: no cached session remains, so the handler must reconstruct the whole chain
  // from the store. A truncated or unbuildable chain would surface as a 404 here.
  auto cold_result = ask_secret("cold branch");
  ASSERT_TRUE(cold_result);
  ASSERT_EQ(cold_result->status, 200) << "Rebuilding the chain from the store failed: " << cold_result->body;

  json cold_response = json::parse(cold_result->body);
  ASSERT_EQ(cold_response["status"], "completed") << cold_result->body;
  EXPECT_EQ(cold_response["previous_response_id"], root_id);
  EXPECT_NE(cold_response["id"], warm_response["id"]);
  ASSERT_TRUE(cold_response.contains("output_text"));

  // Both branches saw the same conversation, so both must be able to recall what only the root turn established.
  EXPECT_NE(ToLower(cold_response["output_text"].get<std::string>()).find("banana"), std::string::npos)
      << "The rebuilt chain lost the root turn. Output: " << cold_response["output_text"];
}

TEST_F(WebServiceIntegrationTest, ResponsesInputItemsExcludeInstructions) {
  auto client = MakeClient();

  json request_body = {
      {"model", model_id()},
      {"instructions", "You are terse."},
      {"input", "Say 'ok'."},
      {"store", true},
      {"max_output_tokens", 256},
      {"temperature", 0},
  };

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  ASSERT_EQ(result->status, 200) << result->body;

  const std::string id = json::parse(result->body)["id"].get<std::string>();

  auto items_result = client.Get(("/v1/responses/" + id + "/input_items").c_str());
  ASSERT_TRUE(items_result) << "HTTP request failed";
  ASSERT_EQ(items_result->status, 200) << items_result->body;

  json items = json::parse(items_result->body);
  ASSERT_TRUE(items.contains("data"));

  // /input_items reports what the caller put in `input`. Instructions are request-scoped state, not an input item.
  ASSERT_EQ(items["data"].size(), 1u) << items_result->body;
  EXPECT_EQ(items["data"][0]["role"], "user");
  for (const auto& item : items["data"]) {
    EXPECT_NE(item.value("role", ""), "system") << "instructions must not be stored as an input item";
  }
}

// ----------------------------------------------------------------------
// Input-item validation: a reasoning item has no role and must be accepted;
// a message item without a role must still be rejected.
// ----------------------------------------------------------------------

TEST_F(WebServiceIntegrationTest, ResponsesAcceptsReasoningInputItem) {
  auto client = MakeClient();

  json request_body = {
      {"model", model_id()},
      {"input", json::array({
                    {{"role", "user"}, {"content", "Think about the number two."}},
                    {{"type", "reasoning"},
                     {"id", "rs_1"},
                     {"summary", json::array({{{"type", "summary_text"}, {"text", "private scratchpad"}}})}},
                    {{"role", "user"}, {"content", "Now say 'ok'."}},
                })},
      {"store", true},
      {"max_output_tokens", 256},
      {"temperature", 0},
  };

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  ASSERT_EQ(result->status, 200) << "A reasoning item echoed back by the caller must be accepted: " << result->body;

  // /input_items reports what the caller sent. The reasoning item is stored exactly as supplied — its text is never
  // replayed into a prompt, but the endpoint is a record of the request, not of what the prompt did with it.
  const std::string id = json::parse(result->body)["id"].get<std::string>();
  auto items_result = client.Get(("/v1/responses/" + id + "/input_items").c_str());
  ASSERT_TRUE(items_result) << "HTTP request failed";
  ASSERT_EQ(items_result->status, 200) << items_result->body;

  json items = json::parse(items_result->body);
  ASSERT_EQ(items["data"].size(), 3u) << items_result->body;

  const json& reasoning = items["data"][1];
  EXPECT_EQ(reasoning.value("type", ""), "reasoning") << items_result->body;
  EXPECT_EQ(reasoning.value("id", ""), "rs_1") << "a supplied item id must not be rewritten";
  ASSERT_TRUE(reasoning.contains("summary")) << items_result->body;
  ASSERT_EQ(reasoning["summary"].size(), 1u);
  EXPECT_EQ(reasoning["summary"][0].value("text", ""), "private scratchpad")
      << "the stored item must round-trip unchanged";
}

TEST_F(WebServiceIntegrationTest, ResponsesRejectsMessageItemWithoutRole) {
  auto client = MakeClient();

  json request_body = {
      {"model", model_id()},
      {"input", json::array({{{"content", "no role here"}}})},
      {"store", false},
  };

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  EXPECT_EQ(result->status, 400) << result->body;
  EXPECT_NE(result->body.find("role"), std::string::npos) << result->body;
}

// ----------------------------------------------------------------------
// Instruction scoping over HTTP.
//
// `instructions` is request-scoped: it never becomes a stored input item, it
// is not carried across a chain, and the value on the current request is the
// only one that applies. This has to hold on both the warm continuation and
// the rebuilt one — the two used to disagree.
// ----------------------------------------------------------------------

TEST_F(WebServiceIntegrationTest, ResponsesInstructionsStayRequestScopedWarmAndCold) {
  auto client = MakeClient();

  auto create = [&](const std::string& instructions, const std::string& input, const std::string& previous_id) {
    json body = {
        {"model", model_id()},
        {"instructions", instructions},
        {"input", input},
        {"store", true},
        {"max_output_tokens", 256},
        {"temperature", 0},
    };

    if (!previous_id.empty()) {
      body["previous_response_id"] = previous_id;
    }

    auto result = client.Post("/v1/responses", body.dump(), "application/json");
    EXPECT_TRUE(result) << "HTTP request failed";
    return result;
  };

  // Every hop's stored input items must be exactly what the caller sent — never a synthesized system message.
  auto expect_no_stored_system_item = [&](const std::string& id) {
    auto items_result = client.Get(("/v1/responses/" + id + "/input_items").c_str());
    ASSERT_TRUE(items_result) << "HTTP request failed";
    ASSERT_EQ(items_result->status, 200) << items_result->body;

    json items = json::parse(items_result->body);
    for (const auto& item : items["data"]) {
      EXPECT_NE(item.value("role", ""), "system") << "hop " << id << " stored instructions as an item";
    }
  };

  auto root = create("You are terse.", "Say 'ok'.", "");
  ASSERT_TRUE(root);
  ASSERT_EQ(root->status, 200) << root->body;
  json root_response = json::parse(root->body);
  EXPECT_EQ(root_response["instructions"], "You are terse.");
  const std::string root_id = root_response["id"].get<std::string>();
  expect_no_stored_system_item(root_id);

  // Warm continuation with the *same* instructions: the cached session keeps its KV cache and the prefix still
  // applies. Consuming the cached session here is what makes the next branch cold.
  auto warm = create("You are terse.", "Say 'ok' again.", root_id);
  ASSERT_TRUE(warm);
  ASSERT_EQ(warm->status, 200) << warm->body;
  json warm_response = json::parse(warm->body);
  EXPECT_EQ(warm_response["instructions"], "You are terse.");
  EXPECT_EQ(warm_response["status"], "completed") << warm->body;
  const std::string warm_id = warm_response["id"].get<std::string>();
  expect_no_stored_system_item(warm_id);

  // This continuation is warm and changes the prefix. It must rebuild the cached generator so the new instructions
  // replace the old prefix instead of leaving the old tokens resident.
  auto warm_changed = create("You are verbose.", "Say 'ok' after changing style.", warm_id);
  ASSERT_TRUE(warm_changed);
  ASSERT_EQ(warm_changed->status, 200) << warm_changed->body;
  json warm_changed_response = json::parse(warm_changed->body);
  EXPECT_EQ(warm_changed_response["instructions"], "You are verbose.");
  EXPECT_EQ(warm_changed_response["status"], "completed") << warm_changed->body;
  expect_no_stored_system_item(warm_changed_response["id"].get<std::string>());

  // Cold continuation from the same root with *different* instructions: the chain is rebuilt from the store and
  // takes its prefix from this request only. The old instructions must not come back with the replayed hops.
  auto cold = create("You are verbose.", "Say 'ok' one more time.", root_id);
  ASSERT_TRUE(cold);
  ASSERT_EQ(cold->status, 200) << "Rebuilding the chain with changed instructions failed: " << cold->body;
  json cold_response = json::parse(cold->body);
  EXPECT_EQ(cold_response["instructions"], "You are verbose.");
  EXPECT_EQ(cold_response["status"], "completed") << cold->body;
  expect_no_stored_system_item(cold_response["id"].get<std::string>());
}

TEST_F(WebServiceIntegrationTest, ResponsesInstructionsAloneCanStartAConversation) {
  auto client = MakeClient();

  json body = {
      {"model", model_id()},
      {"instructions", "Reply with the single word ok."},
      {"input", json::array()},
      {"store", true},
      {"max_output_tokens", 64},
      {"temperature", 0},
  };

  auto result = client.Post("/v1/responses", body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  EXPECT_EQ(response["status"], "completed") << result->body;
  EXPECT_EQ(response["instructions"], "Reply with the single word ok.");
  EXPECT_FALSE(response.value("output_text", "").empty());
}

TEST_F(WebServiceIntegrationTest, ResponsesEmptyInputContinuesWarmAndCold) {
  auto client = MakeClient();

  json root_body = {
      {"model", model_id()},
      {"input", "Reply with the single word ok."},
      {"store", true},
      {"max_output_tokens", 64},
      {"temperature", 0},
  };

  auto root = client.Post("/v1/responses", root_body.dump(), "application/json");
  ASSERT_TRUE(root) << "HTTP request failed";
  ASSERT_EQ(root->status, 200) << root->body;
  const std::string root_id = json::parse(root->body)["id"].get<std::string>();

  auto continue_empty = [&](const char* label) {
    json body = {
        {"model", model_id()},
        {"input", json::array()},
        {"previous_response_id", root_id},
        {"store", true},
        {"max_output_tokens", 64},
        {"temperature", 0},
    };

    auto result = client.Post("/v1/responses", body.dump(), "application/json");
    EXPECT_TRUE(result) << label << ": HTTP request failed";
    return result;
  };

  auto warm = continue_empty("warm");
  ASSERT_TRUE(warm);
  ASSERT_EQ(warm->status, 200) << warm->body;
  EXPECT_EQ(json::parse(warm->body)["status"], "completed");

  auto cold = continue_empty("cold");
  ASSERT_TRUE(cold);
  ASSERT_EQ(cold->status, 200) << cold->body;
  EXPECT_EQ(json::parse(cold->body)["status"], "completed");
}

TEST_F(WebServiceIntegrationTest, ResponsesTrulyEmptyNewConversationIsAClientError) {
  auto client = MakeClient();

  json body = {
      {"model", model_id()},
      {"input", json::array()},
      {"store", false},
  };

  auto result = client.Post("/v1/responses", body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  EXPECT_EQ(result->status, 400) << result->body;
  EXPECT_NE(result->body.find("nothing to generate from"), std::string::npos) << result->body;
}

// ----------------------------------------------------------------------
// Pagination: `has_more` must describe the store, not the page size.
//
// A final page holding exactly `limit` items used to claim a next page that
// did not exist. The store is shared with every other test in this process,
// so the assertions are about the relationship between a page and the page
// that follows it rather than about absolute counts.
// ----------------------------------------------------------------------

TEST_F(WebServiceIntegrationTest, ResponsesListHasMoreAgreesWithTheNextPage) {
  auto client = MakeClient();

  for (int i = 0; i < 3; ++i) {
    json body = {
        {"model", model_id()},
        {"input", "Say 'ok'."},
        {"store", true},
        {"max_output_tokens", 64},
        {"temperature", 0},
    };

    auto result = client.Post("/v1/responses", body.dump(), "application/json");
    ASSERT_TRUE(result) << "HTTP request failed";
    ASSERT_EQ(result->status, 200) << result->body;
  }

  auto page = [&](const std::string& after) {
    std::string url = "/v1/responses?limit=2&order=desc";
    if (!after.empty()) {
      url += "&after=" + after;
    }

    auto result = client.Get(url.c_str());
    EXPECT_TRUE(result) << "HTTP request failed";
    EXPECT_EQ(result->status, 200) << result->body;
    return json::parse(result->body);
  };

  std::string cursor;
  // Bounded walk: the store is capacity-limited, so this always terminates well before the bound.
  for (int guard = 0; guard < 64; ++guard) {
    json current = page(cursor);
    ASSERT_TRUE(current.contains("has_more")) << current.dump();

    const bool has_more = current["has_more"].get<bool>();
    if (current["data"].empty()) {
      EXPECT_FALSE(has_more) << "an empty page cannot have more after it: " << current.dump();
      break;
    }

    const std::string last_id = current["last_id"].get<std::string>();
    json next = page(last_id);

    // The exact property that was broken: a page reporting has_more=false must not be followed by anything, and a
    // page reporting has_more=true must be.
    EXPECT_EQ(has_more, !next["data"].empty())
        << "has_more disagreed with the following page. page=" << current.dump() << " next=" << next.dump();

    if (!has_more) {
      break;
    }

    cursor = last_id;
  }
}
