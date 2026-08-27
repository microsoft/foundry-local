// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Web service integration tests for the /v1/chat/completions endpoint.

#include "web_service_fixture.h"

#include "chat_completions_from_json.h"

TEST_F(WebServiceIntegrationTest, ChatCompletionsBasic) {
  auto client = MakeClient();
  json request_body = {
      {"model", model_id()},
      {"messages", json::array({
                       {{"role", "user"}, {"content", "What is 2+2? Answer with just the number."}},
                   })},
      {"temperature", 0},
      {"max_tokens", 256},
  };

  auto result = client.Post("/v1/chat/completions", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  EXPECT_TRUE(response.contains("id"));
  EXPECT_EQ(response["object"], "chat.completion");
  EXPECT_EQ(response["model"], model_id());

  ASSERT_TRUE(response.contains("choices"));
  ASSERT_EQ(response["choices"].size(), 1u);

  auto& choice = response["choices"][0];
  EXPECT_EQ(choice["index"], 0);
  EXPECT_TRUE(choice.contains("finish_reason"));
  ASSERT_TRUE(choice.contains("message"));
  EXPECT_EQ(choice["message"]["role"], "assistant");

  std::string content = choice["message"]["content"].get<std::string>();
  EXPECT_NE(content.find("4"), std::string::npos)
      << "Expected '4' in response. Got: " << content;

  ASSERT_TRUE(response.contains("usage"));
  EXPECT_GT(response["usage"]["prompt_tokens"].get<int>(), 0);
  EXPECT_GT(response["usage"]["completion_tokens"].get<int>(), 0);
  EXPECT_GT(response["usage"]["total_tokens"].get<int>(), 0);
}

TEST_F(WebServiceIntegrationTest, ChatCompletionsWithParameters) {
  auto client = MakeClient();
  json request_body = {
      {"model", model_id()},
      {"messages", json::array({
                       {{"role", "user"}, {"content", "Say hello."}},
                   })},
      // NOTE: Non-zero temperature triggers ORT GenAI's discrete_distribution
      // sampler assertion with some probability. This is an ORT GenAI bug, not
      // ours. Use temperature=0 (greedy) to keep tests deterministic. The other
      // parameters are still serialized and forwarded, testing the parameter-
      // handling path.
      {"temperature", 0},
      {"top_p", 0.9},
      {"max_tokens", 512},
      {"frequency_penalty", 0.0},
      {"presence_penalty", 0.0},
  };

  auto result = client.Post("/v1/chat/completions", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  ASSERT_TRUE(response.contains("choices"));
  std::string content = response["choices"][0]["message"]["content"].get<std::string>();
  EXPECT_FALSE(content.empty()) << "Response should not be empty";
}

TEST_F(WebServiceIntegrationTest, ChatCompletionsMultiTurn) {
  auto client = MakeClient();
  json request_body = {
      {"model", model_id()},
      {"messages", json::array({
                       {{"role", "user"}, {"content", "What is 2+2?"}},
                       {{"role", "assistant"}, {"content", "4"}},
                       {{"role", "user"}, {"content", "What about 3+3?"}},
                   })},
      {"temperature", 0},
      {"max_tokens", 512},
  };

  auto result = client.Post("/v1/chat/completions", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  std::string content = response["choices"][0]["message"]["content"].get<std::string>();
  std::cout << "Multi-turn output: " << content << "\n";
  EXPECT_NE(content.find("6"), std::string::npos)
      << "Expected '6' in multi-turn response. Got: " << content;
}

TEST_F(WebServiceIntegrationTest, ChatCompletionsStreaming) {
  auto client = MakeClient();
  json request_body = {
      {"model", model_id()},
      {"messages", json::array({
                       {{"role", "user"}, {"content", "Say the word 'hello'."}},
                   })},
      {"temperature", 0},
      {"max_tokens", 512},
      {"stream", true},
      {"stream_options", {{"include_usage", true}}},
  };

  // Use raw POST — streaming returns chunked SSE
  auto result = client.Post("/v1/chat/completions", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;

  // Parse SSE events into typed ChatCompletionChunk structs
  std::vector<fl::ChatCompletionChunk> chunks;
  bool got_done = false;

  std::istringstream stream(result->body);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (line.rfind("data: ", 0) != 0) {
      continue;
    }

    std::string data = line.substr(6);
    if (data == "[DONE]") {
      got_done = true;
      break;
    }

    chunks.push_back(json::parse(data).get<fl::ChatCompletionChunk>());
  }

  ASSERT_TRUE(got_done) << "Stream should end with [DONE]";
  ASSERT_GT(chunks.size(), 1u) << "Expected multiple streaming chunks";

  // All chunks share the same completion id, model, and object type
  for (const auto& chunk : chunks) {
    EXPECT_EQ(chunk.object, "chat.completion.chunk");
    EXPECT_EQ(chunk.model, model_id());
    EXPECT_EQ(chunk.id, chunks[0].id) << "All chunks should share the same completion id";
  }

  // First chunk: role is set to "assistant", content is empty
  ASSERT_FALSE(chunks[0].choices.empty());
  const auto& first_delta = chunks[0].choices[0].delta;
  ASSERT_TRUE(first_delta.role.has_value()) << "First chunk must have role";
  EXPECT_EQ(*first_delta.role, "assistant");
  EXPECT_FALSE(chunks[0].choices[0].finish_reason.has_value()) << "First chunk should not have finish_reason";

  // Middle chunks: accumulate content tokens
  std::string assembled_content;
  for (const auto& chunk : chunks) {
    if (chunk.choices.empty()) {
      continue;
    }

    const auto& delta = chunk.choices[0].delta;
    if (delta.content.has_value()) {
      assembled_content += *delta.content;
    }
  }
  EXPECT_FALSE(assembled_content.empty()) << "Assembled streaming content should not be empty";

  // Find the chunk with finish_reason set
  bool found_finish = false;
  for (const auto& chunk : chunks) {
    if (!chunk.choices.empty() && chunk.choices[0].finish_reason.has_value()) {
      EXPECT_EQ(*chunk.choices[0].finish_reason, "stop");
      found_finish = true;
    }
  }
  EXPECT_TRUE(found_finish) << "Expected a chunk with finish_reason='stop'";

  // Usage chunk (stream_options.include_usage=true)
  bool found_usage = false;
  for (const auto& chunk : chunks) {
    if (chunk.usage.has_value()) {
      const auto& usage = *chunk.usage;
      EXPECT_GT(usage.prompt_tokens, 0);
      EXPECT_GT(usage.completion_tokens, 0);
      EXPECT_EQ(usage.total_tokens, usage.prompt_tokens + usage.completion_tokens);
      found_usage = true;
    }
  }
  EXPECT_TRUE(found_usage) << "Expected a usage chunk when stream_options.include_usage=true";

  std::cout << "Streaming output (" << chunks.size() << " chunks): " << assembled_content << "\n";
}

TEST_F(WebServiceIntegrationTest, ChatCompletionsEmptyBody) {
  auto client = MakeClient();
  auto result = client.Post("/v1/chat/completions", "", "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  EXPECT_EQ(result->status, 400);
}

TEST_F(WebServiceIntegrationTest, ChatCompletionsBadJson) {
  auto client = MakeClient();
  auto result = client.Post("/v1/chat/completions", "{invalid json", "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  EXPECT_EQ(result->status, 400);
}

TEST_F(WebServiceIntegrationTest, ChatCompletionsMissingModel) {
  auto client = MakeClient();
  json request_body = {
      {"messages", json::array({
                       {{"role", "user"}, {"content", "Hello"}},
                   })},
  };

  auto result = client.Post("/v1/chat/completions", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  EXPECT_EQ(result->status, 400);
}

TEST_F(WebServiceIntegrationTest, ChatCompletionsMissingMessages) {
  auto client = MakeClient();
  json request_body = {
      {"model", model_id()},
  };

  auto result = client.Post("/v1/chat/completions", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  EXPECT_EQ(result->status, 400);
}

TEST_F(WebServiceIntegrationTest, ChatCompletionsModelNotFound) {
  auto client = MakeClient();
  json request_body = {
      {"model", "nonexistent_model_xyz"},
      {"messages", json::array({
                       {{"role", "user"}, {"content", "Hello"}},
                   })},
  };

  auto result = client.Post("/v1/chat/completions", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  EXPECT_EQ(result->status, 404);
}

TEST_F(WebServiceIntegrationTest, ChatCompletionsWithResponseFormat) {
  auto client = MakeClient();
  json request_body = {
      {"model", model_id()},
      {"messages", json::array({
                       {{"role", "user"}, {"content", "What is 2+2? Reply with just the number."}},
                   })},
      {"temperature", 0},
      {"max_tokens", 512},
      {"response_format", {{"type", "text"}}},
  };

  auto result = client.Post("/v1/chat/completions", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  ASSERT_TRUE(response.contains("choices"));
  std::string content = response["choices"][0]["message"]["content"].get<std::string>();
  EXPECT_FALSE(content.empty());
}

TEST_F(WebServiceIntegrationTest, ChatCompletionsWithJsonSchemaGuidance) {
  auto client = MakeClient();

  // json_schema response_format should constrain the output to valid JSON matching the schema.
  // This tests that guidance is applied even without tool_choice=required (bug #1042).
  json schema = {
      {"type", "object"},
      {"properties", {
          {"answer", {{"type", "integer"}}},
      }},
      {"required", json::array({"answer"})},
      {"additionalProperties", false},
  };

  json request_body = {
      {"model", model_id()},
      {"messages", json::array({
                       {{"role", "user"}, {"content", "What is 2+2? Respond with JSON."}},
                   })},
      {"temperature", 0},
      {"max_tokens", 64},
      {"response_format", {{"type", "json_schema"}, {"json_schema", schema}}},
  };

  auto result = client.Post("/v1/chat/completions", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed";
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  ASSERT_TRUE(response.contains("choices"));
  std::string content = response["choices"][0]["message"]["content"].get<std::string>();
  EXPECT_FALSE(content.empty()) << "Model produced no output";

  // The output should be valid JSON matching the requested schema exactly.
  json parsed;
  ASSERT_NO_THROW(parsed = json::parse(content)) << "Output is not valid JSON: " << content;

  ASSERT_TRUE(parsed.is_object()) << "Output is not an object: " << content;
  ASSERT_TRUE(parsed.contains("answer")) << "Missing 'answer' field in: " << content;
  EXPECT_TRUE(parsed["answer"].is_number_integer()) << "'answer' is not an integer in: " << content;
  EXPECT_EQ(parsed.size(), 1U) << "Output contains additional properties: " << content;
}
