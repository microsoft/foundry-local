// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Web service integration tests for vision input on the /v1/responses endpoint.
//
// These tests exercise the full pipeline:
//
//   /v1/responses HTTP request
//     -> ResponsesHandler
//     -> ResponseConverter::ToSessionRequest      (decodes data: URL into ImageItem)
//     -> ChatSession::Run                         (routes to vision branch)
//     -> OnnxChatGenerator::CreateWithImages      (ProcessImages + SetInputs)
//     -> Response with non-empty output_text
//
// The whole suite skips when no vision-language-chat model is available so
// that CI on machines without vision-model storage stays green.

#include "model_fixture.h"
#include "utils/temp_path.h"
#include "web_service_fixture.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <utility>

namespace {

// 1x1 transparent PNG. Smallest valid PNG payload — enough to exercise the
// base64-decode + ProcessImages path without bloating the test data dir.
constexpr const char* kTinyPngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkAAIAAAoAAv/lxKUAAAAASUVORK5CYII=";

constexpr const char* kRedPngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAIAAAAlC+aJAAAAb0lEQVR4nO3PAQkAAAyEwO9feoshgnABdLep8QUNyPEFDcjxBQ3I"
    "8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3IPanc"
    "8OLDQitxAAAAAElFTkSuQmCC";
constexpr char kRedPngBytes[] =
    "\x89\x50\x4e\x47\x0d\x0a\x1a\x0a\x00\x00\x00\x0d\x49\x48\x44\x52\x00\x00\x00\x40"
    "\x00\x00\x00\x40\x08\x02\x00\x00\x00\x25\x0b\xe6\x89\x00\x00\x00\x6f\x49\x44\x41"
    "\x54\x78\x9c\xed\xcf\x01\x09\x00\x00\x0c\x84\xc0\xef\x5f\x7a\x8b\x21\x82\x70\x01"
    "\x74\xb7\xa9\xf1\x05\x0d\xc8\xf1\x05\x0d\xc8\xf1\x05\x0d\xc8\xf1\x05\x0d\xc8\xf1"
    "\x05\x0d\xc8\xf1\x05\x0d\xc8\xf1\x05\x0d\xc8\xf1\x05\x0d\xc8\xf1\x05\x0d\xc8\xf1"
    "\x05\x0d\xc8\xf1\x05\x0d\xc8\xf1\x05\x0d\xc8\xf1\x05\x0d\xc8\xf1\x05\x0d\xc8\xf1"
    "\x05\x0d\xc8\xf1\x05\x0d\xc8\xf1\x05\x0d\xc8\xf1\x05\x0d\xc8\xf1\x05\x0d\xc8\xf1"
    "\x05\x0d\xc8\xf1\x05\x0d\xc8\x3d\xa9\xdc\xf0\xe2\xc3\x42\x2b\x71\x00\x00\x00\x00"
    "\x49\x45\x4e\x44\xae\x42\x60\x82";
constexpr const char* kBluePngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAIAAAAlC+aJAAAAb0lEQVR4nO3PAQkAAAyEwO9feoshgnABdNvJ8QUNyPEFDcjxBQ3I"
    "8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ3I8QUNyPEFDcjxBQ2oPcf8"
    "8OIhvJ6vAAAAAElFTkSuQmCC";

json ChatImageContent(const char* png) {
  return json::array({{{"type", "image_url"},
                       {"image_url", {{"url", std::string("data:image/png;base64,") + png}}}}});
}

std::string ReadChatStream(const std::string& body) {
  std::string text;
  bool done = false;
  bool has_usage = false;
  std::istringstream stream(body);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.rfind("data: ", 0) != 0) {
      continue;
    }
    if (line.substr(6) == "[DONE]") {
      done = true;
      break;
    }

    const auto chunk = json::parse(line.substr(6));
    EXPECT_FALSE(chunk.contains("error")) << chunk.dump(2);
    for (const auto& choice : chunk.value("choices", json::array())) {
      text += choice["delta"].value("content", "");
    }
    if (chunk.contains("usage")) {
      has_usage = true;
      EXPECT_GT(chunk["usage"]["prompt_tokens"].get<int>(), 0);
    }
  }

  EXPECT_TRUE(done) << body;
  EXPECT_TRUE(has_usage) << body;
  return text;
}

json ReadCompletedResponse(const std::string& body) {
  json response;
  std::istringstream stream(body);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.rfind("data: ", 0) != 0 || line.substr(6) == "[DONE]") {
      continue;
    }

    const auto event = json::parse(line.substr(6));
    const auto type = event.value("type", "");
    EXPECT_NE(type, "response.failed") << body;
    if (type == "response.completed" || type == "response.incomplete") {
      response = event.at("response");
    }
  }

  return response;
}

}  // namespace

// ----------------------------------------------------------------------
// Fixture: web service + vision model. Skips when either is unavailable.
// ----------------------------------------------------------------------

class ResponsesVisionIntegrationTest : public WebServiceFixture {
 protected:
  static void SetUpTestSuite() {
    SharedTestEnv::Get().AcquireModels({SharedTestEnv::Modality::Vision});
  }

  void SetUp() override {
    auto& env = SharedTestEnv::Get();
    if (!fl::test::SafeGetEnv("FOUNDRY_LOCAL_VISION_TEST_MODEL_PATH").empty()) {
      ASSERT_NE(env.vision_model(), nullptr) << "The explicitly supplied cached vision model must load";
    }
    if (!env.vision_model()) {
      GTEST_SKIP() << "No vision-language-chat model available";
    }
  }

  static const std::string& vision_model_id() {
    return SharedTestEnv::Get().vision_model_id();
  }

  static std::string VisionEndpoint(const char* path) {
    return SharedTestEnv::Get().vision_endpoint(path);
  }
};

TEST_F(ResponsesVisionIntegrationTest, DataUrlImageProducesOutput) {
  auto client = MakeClient();
  // Vision inference (image preprocessing + first-token-latency on a
  // multi-billion-parameter model) routinely exceeds the 60s default. Bump
  // it well above worst-case observed cold-start latency.
  client.set_read_timeout(600, 0);

  // Content-array input with input_text + input_image, the OpenAI Responses
  // API shape that the converter routes through the vision pipeline.
  json request_body = {
      {"model", vision_model_id()},
      {"input", json::array({
                    {{"role", "user"},
                     {"content", json::array({
                                     {{"type", "input_text"},
                                      {"text", "Describe this image in one short sentence."}},
                                     {{"type", "input_image"},
                                      {"detail", "low"},
                                      {"image_url", std::string("data:image/png;base64,") + kTinyPngBase64}},
                                 })}},
                })},
      {"max_output_tokens", 512},
      {"temperature", 0},
      {"store", false},
  };

  auto result = client.Post(VisionEndpoint("/v1/responses"), request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  EXPECT_EQ(response["object"], "response");
  EXPECT_EQ(response["status"], "completed") << response.dump(2);
  EXPECT_EQ(response["model"], vision_model_id());

  // The model must have produced *something* — we don't assert on content
  // because a 1x1 PNG provides no real signal, but the vision branch must
  // run end-to-end without throwing.
  ASSERT_TRUE(response.contains("output_text"));
  EXPECT_FALSE(response["output_text"].get<std::string>().empty())
      << "Vision pipeline produced an empty output_text";

  ASSERT_TRUE(response.contains("usage"));
  EXPECT_GT(response["usage"]["input_tokens"].get<int>(), 0);
  EXPECT_GT(response["usage"]["output_tokens"].get<int>(), 0);
}

TEST_F(ResponsesVisionIntegrationTest, ChatCompletionsUsesImageColorIncludingStreaming) {
  auto client = MakeClient(600);
  for (const bool streaming : {false, true}) {
    for (const auto& [png, color] : {std::pair{kRedPngBase64, "red"}, std::pair{kBluePngBase64, "blue"}}) {
      SCOPED_TRACE(std::string(color) + (streaming ? " streaming" : " nonstreaming"));
      auto content = ChatImageContent(png);
      content.insert(content.begin(),
                     json{{"type", "text"}, {"text", "What color is this image? Answer with one word."}});
      const json body = {
          {"model", vision_model_id()},
          {"messages", json::array({{{"role", "user"}, {"content", content}}})},
          {"max_completion_tokens", 32},
          {"temperature", 0},
          {"reasoning_effort", "none"},
          {"stream", streaming},
          {"stream_options", {{"include_usage", true}}},
      };
      const auto result = client.Post(VisionEndpoint("/v1/chat/completions"), body.dump(), "application/json");
      ASSERT_TRUE(result) << httplib::to_string(result.error());
      ASSERT_EQ(result->status, 200) << result->body;
      const auto text = streaming ? ReadChatStream(result->body)
                                  : json::parse(result->body)["choices"][0]["message"]["content"].get<std::string>();
      const auto normalized = fl::test::ToLower(text);
      EXPECT_TRUE(std::regex_search(normalized, std::regex("\\b" + std::string(color) + "\\b"))) << text;
      const auto other_color = std::string(color) == "red" ? "blue" : "red";
      EXPECT_FALSE(std::regex_search(normalized, std::regex("\\b" + std::string(other_color) + "\\b"))) << text;
    }
  }
}

TEST_F(ResponsesVisionIntegrationTest, ChatCompletionsRetainsImagesAcrossUserTurns) {
  auto client = MakeClient(600);
  for (const bool streaming : {false, true}) {
    for (const bool reverse : {false, true}) {
      SCOPED_TRACE(std::string(streaming ? "streaming" : "nonstreaming") + (reverse ? " blue first" : " red first"));
      auto first = ChatImageContent(reverse ? kBluePngBase64 : kRedPngBase64);
      first.insert(first.begin(), json{{"type", "text"}, {"text", "Picture 1. Remember this image. Reply only OK."}});
      auto second = ChatImageContent(reverse ? kRedPngBase64 : kBluePngBase64);
      second.insert(second.begin(),
                    json{{"type", "text"},
                         {"text", "Picture 2. What color was the first image? Answer with one word."}});
      const json body = {
          {"model", vision_model_id()},
          {"messages", json::array({{{"role", "user"}, {"content", first}},
                                    {{"role", "assistant"}, {"content", "OK"}},
                                    {{"role", "user"}, {"content", second}}})},
          {"max_completion_tokens", 32},
          {"temperature", 0},
          {"reasoning_effort", "none"},
          {"stream", streaming},
          {"stream_options", {{"include_usage", true}}},
      };
      const auto result = client.Post(VisionEndpoint("/v1/chat/completions"), body.dump(), "application/json");
      ASSERT_TRUE(result) << httplib::to_string(result.error());
      ASSERT_EQ(result->status, 200) << result->body;
      const auto text = streaming ? ReadChatStream(result->body)
                                  : json::parse(result->body)["choices"][0]["message"]["content"].get<std::string>();
      const auto normalized = fl::test::ToLower(text);
      EXPECT_TRUE(std::regex_search(normalized, std::regex(reverse ? "\\bblue\\b" : "\\bred\\b"))) << text;
      EXPECT_FALSE(std::regex_search(normalized, std::regex(reverse ? "\\bred\\b" : "\\bblue\\b"))) << text;
    }
  }
}

TEST_F(ResponsesVisionIntegrationTest, CancelledAndUndoneImageTurnsLeaveRetainedHistoryIntact) {
  using namespace foundry_local;

  ChatSession session(*SharedTestEnv::Get().vision_model());
  RequestOptions options;
  options.search.temperature = 0.0f;
  options.search.max_output_tokens = 32;
  options.additional_options.Set("chat_template_kwargs", R"({"enable_thinking":false})");
  session.SetOptions(options);

  const auto image_request = [](const char* text) {
    std::vector<Item> content;
    content.push_back(Item::Text(text));
    content.push_back(Item::ImageFromData("png", kRedPngBytes, sizeof(kRedPngBytes) - 1));
    return Request{MessageItem(FOUNDRY_LOCAL_ROLE_USER, std::move(content))};
  };
  const auto first = image_request("Remember this image. Reply only OK.");
  const auto first_text = fl::test::ToLower(CollectResponseText(session.ProcessRequest(first)));
  ASSERT_EQ(session.TurnCount(), 1u);
  ASSERT_EQ(first_text.find("red"), std::string::npos) << first_text;

  const Request probe{UserMessage("What color was the image? Answer with one word.")};
  const auto baseline = session.PreflightRequest(probe);
  ASSERT_TRUE(baseline.fits);

  const auto second = image_request("Describe the second image in one sentence.");
  std::atomic<int> callbacks{0};
  session.SetStreamingCallback([&](Item) {
    ++callbacks;
    return 1;
  });
  try {
    (void)session.ProcessRequest(second);
    FAIL() << "Expected streaming callback cancellation";
  } catch (const Error& error) {
    EXPECT_EQ(error.Code(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED) << error.what();
  }
  session.SetStreamingCallback(nullptr);
  EXPECT_GT(callbacks.load(), 0);
  EXPECT_EQ(session.TurnCount(), 1u);
  EXPECT_EQ(session.PreflightRequest(probe).prompt_tokens, baseline.prompt_tokens);

  (void)session.ProcessRequest(second);
  ASSERT_EQ(session.TurnCount(), 2u);
  session.UndoTurns(1);
  EXPECT_EQ(session.TurnCount(), 1u);
  EXPECT_EQ(session.PreflightRequest(probe).prompt_tokens, baseline.prompt_tokens);
  const auto answer = CollectResponseText(session.ProcessRequest(probe));
  EXPECT_TRUE(std::regex_search(fl::test::ToLower(answer), std::regex("\\bred\\b"))) << answer;
}

TEST_F(ResponsesVisionIntegrationTest, ChatCompletionsImageOnlyProducesOutput) {
  auto client = MakeClient(600);
  const json body = {
      {"model", vision_model_id()},
      {"messages", json::array({{{"role", "user"}, {"content", ChatImageContent(kTinyPngBase64)}}})},
      {"max_completion_tokens", 32},
      {"temperature", 0},
  };
  const auto result = client.Post(VisionEndpoint("/v1/chat/completions"), body.dump(), "application/json");
  ASSERT_TRUE(result) << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;
  const auto response = json::parse(result->body);
  EXPECT_EQ(response["object"], "chat.completion");
  EXPECT_FALSE(response["choices"][0]["message"]["content"].get<std::string>().empty()) << response.dump(2);
  EXPECT_GT(response["usage"]["prompt_tokens"].get<int>(), 0);
  EXPECT_GT(response["usage"]["completion_tokens"].get<int>(), 0);
}

TEST_F(ResponsesVisionIntegrationTest, ChatCompletionsImageOnlyStreamsOutput) {
  auto client = MakeClient(600);
  const json body = {
      {"model", vision_model_id()},
      {"messages",
       json::array(
           {{{"role", "user"},
             {"content",
              json::array({{{"type", "image_url"},
                            {"image_url", {{"url", std::string("data:image/png;base64,") + kTinyPngBase64}}}}})}}})},
      {"max_completion_tokens", 32},
      {"temperature", 0},
      {"stream", true},
      {"stream_options", {{"include_usage", true}}},
  };
  const auto result = client.Post(VisionEndpoint("/v1/chat/completions"), body.dump(), "application/json");
  ASSERT_TRUE(result) << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;

  EXPECT_FALSE(ReadChatStream(result->body).empty()) << result->body;
}

TEST_F(ResponsesVisionIntegrationTest, ChatJsonImageOnlyPreflightMatchesExecutionBudget) {
  using namespace foundry_local;
  for (const bool explicit_limit : {false, true}) {
    SCOPED_TRACE(explicit_limit ? "explicit output limit" : "default media output limit");
    ChatSession session(*SharedTestEnv::Get().vision_model());
    json body = {
        {"model", vision_model_id()},
        {"messages", json::array({{{"role", "user"}, {"content", ChatImageContent(kTinyPngBase64)}}})},
        {"temperature", 0},
    };
    if (explicit_limit) {
      body["max_completion_tokens"] = 8;
    }

    Request request;
    request.AddItem(Item::Text(body.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
    const auto budget = session.PreflightRequest(request);
    ASSERT_TRUE(budget.fits);
    ASSERT_GT(budget.prompt_tokens, 0);
    EXPECT_EQ(budget.output_reserve_tokens, explicit_limit ? 8 : 3072);
    const auto response = session.ProcessRequest(request);
    const auto usage = response.GetUsage();
    EXPECT_EQ(usage.prompt_tokens, budget.prompt_tokens);
    EXPECT_GT(usage.completion_tokens, 0);
    EXPECT_LE(usage.completion_tokens, budget.output_reserve_tokens);

    body["max_completion_tokens"] = budget.context_limit_tokens - budget.prompt_tokens + 1;
    Request oversized;
    oversized.AddItem(Item::Text(body.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));
    const auto rejected = session.PreflightRequest(oversized);
    EXPECT_EQ(rejected.prompt_tokens, budget.prompt_tokens);
    EXPECT_EQ(rejected.required_tokens, budget.context_limit_tokens + 1);
    EXPECT_EQ(rejected.deficit_tokens, 1);
    EXPECT_FALSE(rejected.fits);
    try {
      (void)session.ProcessRequest(oversized);
      FAIL() << "Expected the image prompt plus output reserve to exceed the context limit";
    } catch (const Error& error) {
      EXPECT_EQ(error.Code(), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT) << error.what();
    }
  }
}

TEST_F(ResponsesVisionIntegrationTest, ImageOnlyMessageProducesOutput) {
  auto client = MakeClient();
  client.set_read_timeout(600, 0);

  json request_body = {
      {"model", vision_model_id()},
      {"input", json::array({
                    {{"role", "user"},
                     {"content", json::array({
                                     {{"type", "input_image"},
                                      {"detail", "low"},
                                      {"image_url", std::string("data:image/png;base64,") + kTinyPngBase64}},
                                 })}},
                })},
      {"max_output_tokens", 128},
      {"temperature", 0},
      {"store", false},
  };

  auto result = client.Post(VisionEndpoint("/v1/responses"), request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  EXPECT_EQ(response["status"], "completed") << response.dump(2);
  EXPECT_FALSE(response.value("output_text", "").empty());
}

TEST_F(ResponsesVisionIntegrationTest, LegacyImageDataProducesOutput) {
  auto client = MakeClient();
  client.set_read_timeout(600, 0);

  json request_body = {
      {"model", vision_model_id()},
      {"input", json::array({
                    {{"role", "user"},
                     {"content", json::array({
                                     {{"type", "input_text"},
                                      {"text", "Describe this image in one short sentence."}},
                                     {{"type", "input_image"},
                                      {"detail", "low"},
                                      {"image_data", kTinyPngBase64},
                                      {"media_type", "image/png"}},
                                 })}},
                })},
      {"max_output_tokens", 512},
      {"temperature", 0},
      {"store", false},
  };

  auto result = client.Post(VisionEndpoint("/v1/responses"), request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  EXPECT_EQ(response["status"], "completed") << response.dump(2);
  ASSERT_TRUE(response.contains("output_text"));
  EXPECT_FALSE(response["output_text"].get<std::string>().empty());
}

TEST_F(ResponsesVisionIntegrationTest, RemoteHttpUrlIsRejected) {
  auto client = MakeClient();

  // http(s) URLs are explicitly NOT supported (the converter rejects them
  // with NOT_IMPLEMENTED). Verify the service surfaces this as an error
  // rather than crashing or hanging.
  json request_body = {
      {"model", vision_model_id()},
      {"input", json::array({
                    {{"role", "user"},
                     {"content", json::array({
                                     {{"type", "input_text"}, {"text", "Describe this."}},
                                     {{"type", "input_image"},
                                      {"detail", "auto"},
                                      {"image_url", "https://example.com/does-not-matter.png"}},
                                 })}},
                })},
      {"max_output_tokens", 16},
      {"store", false},
  };

  auto result = client.Post(VisionEndpoint("/v1/responses"), request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  // The converter throws NOT_IMPLEMENTED → service maps that to a 4xx/5xx,
  // not a 200. Exact code is the handler's choice; the contract is "not 200".
  EXPECT_NE(result->status, 200) << "Expected error for unsupported http(s) image URL: " << result->body;
}

TEST_F(ResponsesVisionIntegrationTest, TwoImagesProduceOutput) {
  auto client = MakeClient();
  client.set_read_timeout(600, 0);

  json request_body = {
      {"model", vision_model_id()},
      {"input", json::array({
                    {{"role", "user"},
                     {"content", json::array({
                                     {{"type", "input_text"},
                                      {"text", "Describe both images in one short sentence."}},
                                     {{"type", "input_image"},
                                      {"detail", "low"},
                                      {"image_url", std::string("data:image/png;base64,") + kTinyPngBase64}},
                                     {{"type", "input_image"},
                                      {"detail", "low"},
                                      {"image_url", std::string("data:image/png;base64,") + kTinyPngBase64}},
                                 })}},
                })},
      {"max_output_tokens", 512},
      {"temperature", 0},
      {"store", false},
  };

  auto result = client.Post(VisionEndpoint("/v1/responses"), request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  EXPECT_EQ(response["status"], "completed") << response.dump(2);
  EXPECT_EQ(response["model"], vision_model_id());
  ASSERT_TRUE(response.contains("output_text"));
  EXPECT_FALSE(response["output_text"].get<std::string>().empty())
      << "Two-image request produced an empty output_text";

  ASSERT_TRUE(response.contains("usage"));
  EXPECT_GT(response["usage"]["input_tokens"].get<int>(), 0);
  EXPECT_GT(response["usage"]["output_tokens"].get<int>(), 0);
}

// ----------------------------------------------------------------------
// Media policy: conversation-scoped, and independent of cache residency.
// ----------------------------------------------------------------------

TEST_F(ResponsesVisionIntegrationTest, ImageWithDeclaredToolsIsRejected) {
  auto client = MakeClient();

  // The multimodal prompt does not represent tool calls or results, so reject this combination before generation.
  json request_body = {
      {"model", vision_model_id()},
      {"input", json::array({
                    {{"role", "user"},
                     {"content", json::array({
                                     {{"type", "input_text"}, {"text", "What is in this image?"}},
                                     {{"type", "input_image"},
                                      {"detail", "low"},
                                      {"image_url", std::string("data:image/png;base64,") + kTinyPngBase64}},
                                 })}},
                })},
      {"tools", json::array({{{"type", "function"},
                              {"name", "get_weather"},
                              {"description", "Get the weather"},
                              {"parameters", {{"type", "object"}, {"properties", json::object()}}}}})},
      {"max_output_tokens", 64},
      {"store", false},
  };

  auto result = client.Post(VisionEndpoint("/v1/responses"), request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  EXPECT_EQ(result->status, 400) << "media + declared tools must be a client error: " << result->body;
  EXPECT_NE(result->body.find("tool definitions"), std::string::npos) << result->body;
}

TEST_F(ResponsesVisionIntegrationTest, ImagesAndTextContinueWarmAndColdConversations) {
  auto client = MakeClient();
  client.set_read_timeout(600, 0);

  json first = {
      {"model", vision_model_id()},
      {"input", "Say 'ok'."},
      {"store", true},
      {"max_output_tokens", 64},
      {"temperature", 0},
      {"reasoning", {{"effort", "none"}}},
  };

  auto first_result = client.Post(VisionEndpoint("/v1/responses"), first.dump(), "application/json");
  ASSERT_TRUE(first_result) << "HTTP request failed";
  ASSERT_EQ(first_result->status, 200) << first_result->body;
  const std::string first_id = json::parse(first_result->body)["id"].get<std::string>();

  const json red_turn = {
      {"model", vision_model_id()},
      {"previous_response_id", first_id},
      {"input", json::array({{{"role", "user"},
                              {"content", json::array({{{"type", "input_text"},
                                                        {"text", "Picture 1. Remember this image. Reply only OK."}},
                                                       {{"type", "input_image"},
                                                        {"image_url", std::string("data:image/png;base64,") +
                                                                          kRedPngBase64}}})}}})},
      {"max_output_tokens", 32},
      {"temperature", 0},
      {"reasoning", {{"effort", "none"}}},
      {"store", true},
  };
  const auto red_result = client.Post(VisionEndpoint("/v1/responses"), red_turn.dump(), "application/json");
  ASSERT_TRUE(red_result);
  ASSERT_EQ(red_result->status, 200) << red_result->body;
  const auto red_response = json::parse(red_result->body);
  const auto remembered_reply = fl::test::ToLower(red_response.value("output_text", ""));
  ASSERT_EQ(remembered_reply.find("red"), std::string::npos) << remembered_reply;
  ASSERT_EQ(remembered_reply.find("blue"), std::string::npos) << remembered_reply;
  const auto red_id = red_response["id"].get<std::string>();

  const json blue_turn = {
      {"model", vision_model_id()},
      {"previous_response_id", red_id},
      {"input", json::array({{{"role", "user"},
                              {"content", json::array({{{"type", "input_text"},
                                                        {"text", "Picture 2. What color was the first image? One word."}},
                                                       {{"type", "input_image"},
                                                        {"image_url", std::string("data:image/png;base64,") +
                                                                          kBluePngBase64}}})}}})},
      {"max_output_tokens", 32},
      {"temperature", 0},
      {"reasoning", {{"effort", "none"}}},
      {"store", false},
  };
  for (const char* path : {"warm", "cold"}) {
    SCOPED_TRACE(path);
    const auto result = client.Post(VisionEndpoint("/v1/responses"), blue_turn.dump(), "application/json");
    ASSERT_TRUE(result);
    ASSERT_EQ(result->status, 200) << result->body;
    const auto text = json::parse(result->body).value("output_text", "");
    const auto normalized = fl::test::ToLower(text);
    EXPECT_TRUE(std::regex_search(normalized, std::regex("\\bred\\b"))) << text;
    EXPECT_FALSE(std::regex_search(normalized, std::regex("\\bblue\\b"))) << text;
  }

  const json text_turn = {{"model", vision_model_id()},
                          {"previous_response_id", red_id},
                          {"input", "What color was the image? Answer with one word."},
                          {"max_output_tokens", 32},
                          {"temperature", 0},
                          {"reasoning", {{"effort", "none"}}},
                          {"store", false}};
  const auto text_result = client.Post(VisionEndpoint("/v1/responses"), text_turn.dump(), "application/json");
  ASSERT_TRUE(text_result);
  ASSERT_EQ(text_result->status, 200) << text_result->body;
  const auto text = json::parse(text_result->body).value("output_text", "");
  EXPECT_TRUE(std::regex_search(fl::test::ToLower(text), std::regex("\\bred\\b"))) << text;
}

TEST_F(ResponsesVisionIntegrationTest, LocalImageReplaySurvivesDeletionForStreamingAndNonStreaming) {
  auto client = MakeClient(600);
  for (const bool streaming : {false, true}) {
    SCOPED_TRACE(streaming ? "streaming" : "nonstreaming");
    const auto dir = fl::test::TempPath::CreateTempDir("fl_vision_replay_");
    const auto image_path = dir.path() / "red.png";
    {
      std::ofstream file(image_path, std::ios::binary);
      file.write(kRedPngBytes, static_cast<std::streamsize>(sizeof(kRedPngBytes) - 1));
      ASSERT_TRUE(file.good());
    }

    const json first = {
        {"model", vision_model_id()},
        {"input", json::array({{{"role", "user"},
                                {"content", json::array({{{"type", "input_text"},
                                                          {"text", "Remember this image. Reply only OK."}},
                                                         {{"type", "input_image"},
                                                          {"image_url", image_path.string()}}})}}})},
        {"stream", streaming},
        {"store", true},
        {"max_output_tokens", 32},
        {"temperature", 0},
        {"reasoning", {{"effort", "none"}}},
    };
    const auto first_result = client.Post(VisionEndpoint("/v1/responses"), first.dump(), "application/json");
    ASSERT_TRUE(first_result);
    ASSERT_EQ(first_result->status, 200) << first_result->body;
    const auto first_response = streaming ? ReadCompletedResponse(first_result->body) : json::parse(first_result->body);
    ASSERT_TRUE(first_response.is_object()) << first_result->body;
    const auto id = first_response.at("id").get<std::string>();
    const auto first_text = fl::test::ToLower(first_response.value("output_text", ""));
    ASSERT_EQ(first_text.find("red"), std::string::npos) << first_text;
    ASSERT_TRUE(std::filesystem::remove(image_path));

    const auto public_items = client.Get(VisionEndpoint("/v1/responses") + "/" + id + "/input_items");
    ASSERT_TRUE(public_items);
    ASSERT_EQ(public_items->status, 200) << public_items->body;
    EXPECT_EQ(json::parse(public_items->body)["data"][0]["content"][1]["image_url"], image_path.string());

    const json followup = {{"model", vision_model_id()},
                           {"previous_response_id", id},
                           {"input", "What color was the image? Answer with one word."},
                           {"store", false},
                           {"max_output_tokens", 32},
                           {"temperature", 0},
                           {"reasoning", {{"effort", "none"}}}};
    for (const char* path : {"warm", "cold"}) {
      SCOPED_TRACE(path);
      const auto result = client.Post(VisionEndpoint("/v1/responses"), followup.dump(), "application/json");
      ASSERT_TRUE(result);
      ASSERT_EQ(result->status, 200) << result->body;
      const auto text = json::parse(result->body).value("output_text", "");
      EXPECT_TRUE(std::regex_search(fl::test::ToLower(text), std::regex("\\bred\\b"))) << text;
    }
  }
}
