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
#include "web_service_fixture.h"

namespace {

// 1x1 transparent PNG. Smallest valid PNG payload — enough to exercise the
// base64-decode + ProcessImages path without bloating the test data dir.
constexpr const char* kTinyPngBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkAAIAAAoAAv/lxKUAAAAASUVORK5CYII=";

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
    if (!env.vision_model()) {
      GTEST_SKIP() << "No vision-language-chat model available";
    }
  }

  static const std::string& vision_model_id() {
    return SharedTestEnv::Get().vision_model_id();
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

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");
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

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  EXPECT_EQ(response["status"], "completed") << response.dump(2);
  EXPECT_FALSE(response.value("output_text", "").empty());
}

TEST_F(ResponsesVisionIntegrationTest, ImageDataProducesOutput) {
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

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");
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

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");
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
                                      {"image_data", kTinyPngBase64},
                                      {"media_type", "image/png"}},
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

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");
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

  // The model could answer an image turn with a tool call, and the turn carrying that call's result could no longer
  // show it the image. The combination is rejected before any generation happens.
  json request_body = {
      {"model", vision_model_id()},
      {"input", json::array({
                    {{"role", "user"},
                     {"content", json::array({
                                     {{"type", "input_text"}, {"text", "What is in this image?"}},
                                     {{"type", "input_image"},
                                      {"detail", "low"},
                                      {"image_data", kTinyPngBase64},
                                      {"media_type", "image/png"}},
                                 })}},
                })},
      {"tools", json::array({{{"type", "function"},
                              {"name", "get_weather"},
                              {"description", "Get the weather"},
                              {"parameters", {{"type", "object"}, {"properties", json::object()}}}}})},
      {"max_output_tokens", 64},
      {"store", false},
  };

  auto result = client.Post("/v1/responses", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  EXPECT_EQ(result->status, 400) << "media + declared tools must be a client error: " << result->body;
  EXPECT_NE(result->body.find("tool definitions"), std::string::npos) << result->body;
}

TEST_F(ResponsesVisionIntegrationTest, ImageOnAContinuationIsRejected) {
  auto client = MakeClient();
  client.set_read_timeout(600, 0);

  json first = {
      {"model", vision_model_id()},
      {"input", "Say 'ok'."},
      {"store", true},
      {"max_output_tokens", 64},
      {"temperature", 0},
  };

  auto first_result = client.Post("/v1/responses", first.dump(), "application/json");
  ASSERT_TRUE(first_result) << "HTTP request failed";
  ASSERT_EQ(first_result->status, 200) << first_result->body;
  const std::string first_id = json::parse(first_result->body)["id"].get<std::string>();

  auto send_image_continuation = [&]() {
    json body = {
        {"model", vision_model_id()},
        {"previous_response_id", first_id},
        {"input", json::array({
                      {{"role", "user"},
                       {"content", json::array({
                                       {{"type", "input_text"}, {"text", "And this image?"}},
                                       {{"type", "input_image"},
                                        {"detail", "low"},
                                        {"image_data", kTinyPngBase64},
                                        {"media_type", "image/png"}},
                                   })}},
                  })},
        {"max_output_tokens", 64},
        {"store", false},
    };
    return client.Post("/v1/responses", body.dump(), "application/json");
  };

  // Warm: the session cached under first_id still holds the conversation.
  auto warm = send_image_continuation();
  ASSERT_TRUE(warm) << "HTTP request failed";
  EXPECT_EQ(warm->status, 400) << "media on a continuation must be a client error: " << warm->body;
  EXPECT_NE(warm->body.find("first turn of a conversation"), std::string::npos) << warm->body;

  // Cold: that request checked the session out and the failure did not put it back, so this one rebuilds the chain
  // from the store. The rejection must be identical.
  auto cold = send_image_continuation();
  ASSERT_TRUE(cold) << "HTTP request failed";
  EXPECT_EQ(cold->status, warm->status) << "warm and cold must reject a media continuation identically: "
                                        << cold->body;
  EXPECT_NE(cold->body.find("first turn of a conversation"), std::string::npos) << cold->body;
}
