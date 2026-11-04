// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "contracts/embeddings.h"
#include "inferencing/generative/embeddings/embeddings_session.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <variant>
#include <vector>

using namespace fl;

TEST(EmbeddingBatchCancellationTest, CancellationStopsBeforeNextInputAndAllowsReuse) {
  Request request;
  ASSERT_TRUE(request.TryBegin());
  int calls = 0;
  const auto generate = [&](const std::string&) {
    ++calls;
    request.Cancel();
    return std::vector<float>{1.0f};
  };

  try {
    embeddings_internal::GenerateBatch({"first", "second", "third"}, request, generate);
    FAIL() << "Canceled batch unexpectedly completed";
  } catch (const Exception& error) {
    EXPECT_EQ(error.code(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  }

  EXPECT_EQ(calls, 1);
  request.PublishCompletion();
  ASSERT_TRUE(request.TryBegin());
  const auto results = embeddings_internal::GenerateBatch(
      {"first", "second"}, request, [](const std::string&) { return std::vector<float>{1.0f}; });
  EXPECT_EQ(results.size(), 2u);
  request.PublishCompletion();
}

TEST(EmbeddingBatchCancellationTest, CancellationDuringLastInputDoesNotPublishResults) {
  Request request;
  ASSERT_TRUE(request.TryBegin());
  EXPECT_THROW(embeddings_internal::GenerateBatch({"only"}, request, [&](const std::string&) {
                 request.Cancel();
                 return std::vector<float>{1.0f};
               }),
               Exception);
  request.PublishCompletion();
}

TEST(EmbeddingBatchCancellationTest, CanceledRequestDoesNotStartForwardPass) {
  Request request;
  ASSERT_TRUE(request.TryBegin());
  request.Cancel();
  int calls = 0;
  EXPECT_THROW(embeddings_internal::GenerateBatch({"first"}, request, [&](const std::string&) {
                 ++calls;
                 return std::vector<float>{1.0f};
               }),
               Exception);
  EXPECT_EQ(calls, 0);
  request.PublishCompletion();
}

// ===========================================================================
// Request parse tests
// ===========================================================================

TEST(EmbeddingCreateRequestTest, Parse_StringInput) {
  auto j = nlohmann::json::parse(R"({"model":"m","input":"hello"})");
  auto req = j.get<EmbeddingCreateRequest>();

  EXPECT_EQ(req.model, "m");
  ASSERT_TRUE(std::holds_alternative<std::string>(req.input));
  EXPECT_EQ(std::get<std::string>(req.input), "hello");
  EXPECT_FALSE(req.encoding_format.has_value());
}

TEST(EmbeddingCreateRequestTest, Parse_ArrayInput) {
  auto j = nlohmann::json::parse(R"({"model":"m","input":["a","b","c"]})");
  auto req = j.get<EmbeddingCreateRequest>();

  EXPECT_EQ(req.model, "m");
  ASSERT_TRUE(std::holds_alternative<std::vector<std::string>>(req.input));
  const auto& v = std::get<std::vector<std::string>>(req.input);
  ASSERT_EQ(v.size(), 3u);
  EXPECT_EQ(v[0], "a");
  EXPECT_EQ(v[1], "b");
  EXPECT_EQ(v[2], "c");
}

TEST(EmbeddingCreateRequestTest, Parse_WithEncodingFormat) {
  auto j = nlohmann::json::parse(R"({"model":"m","input":"x","encoding_format":"float"})");
  auto req = j.get<EmbeddingCreateRequest>();

  ASSERT_TRUE(req.encoding_format.has_value());
  EXPECT_EQ(*req.encoding_format, "float");
}

TEST(EmbeddingCreateRequestTest, Parse_MissingModel_Throws) {
  auto j = nlohmann::json::parse(R"({"input":"x"})");
  EXPECT_THROW(j.get<EmbeddingCreateRequest>(), nlohmann::json::out_of_range);
}

TEST(EmbeddingCreateRequestTest, Parse_MissingInput_Throws) {
  auto j = nlohmann::json::parse(R"({"model":"m"})");
  EXPECT_THROW(j.get<EmbeddingCreateRequest>(), nlohmann::json::out_of_range);
}

TEST(EmbeddingCreateRequestTest, Parse_InputWrongType_Throws) {
  auto j = nlohmann::json::parse(R"({"model":"m","input":42})");
  EXPECT_THROW(j.get<EmbeddingCreateRequest>(), nlohmann::json::type_error);
}

// ===========================================================================
// Response serialize tests
// ===========================================================================

TEST(EmbeddingCreateResponseTest, Serialize_EmptyResponse_HasOpenAIShape) {
  EmbeddingCreateResponse resp;
  resp.model = "test-model";

  nlohmann::json j = resp;

  EXPECT_EQ(j["object"], "list");
  EXPECT_EQ(j["model"], "test-model");
  ASSERT_TRUE(j["data"].is_array());
  EXPECT_TRUE(j["data"].empty());
  ASSERT_TRUE(j.contains("usage"));
  EXPECT_EQ(j["usage"]["prompt_tokens"], 0);
  EXPECT_EQ(j["usage"]["total_tokens"], 0);
}

TEST(EmbeddingCreateResponseTest, Serialize_SingleEmbedding) {
  EmbeddingCreateResponse resp;
  resp.model = "m";
  EmbeddingData d;
  d.embedding = {0.1f, 0.2f, 0.3f};
  d.index = 0;
  resp.data.push_back(std::move(d));

  nlohmann::json j = resp;

  ASSERT_EQ(j["data"].size(), 1u);
  EXPECT_EQ(j["data"][0]["object"], "embedding");
  EXPECT_EQ(j["data"][0]["index"], 0);
  ASSERT_TRUE(j["data"][0]["embedding"].is_array());
  ASSERT_EQ(j["data"][0]["embedding"].size(), 3u);
  EXPECT_FLOAT_EQ(j["data"][0]["embedding"][0].get<float>(), 0.1f);
  EXPECT_FLOAT_EQ(j["data"][0]["embedding"][1].get<float>(), 0.2f);
  EXPECT_FLOAT_EQ(j["data"][0]["embedding"][2].get<float>(), 0.3f);
}

TEST(EmbeddingCreateResponseTest, Serialize_MultipleEmbeddings) {
  EmbeddingCreateResponse resp;
  resp.model = "m";

  EmbeddingData d0;
  d0.embedding = {1.0f, 2.0f};
  d0.index = 0;
  resp.data.push_back(std::move(d0));

  EmbeddingData d1;
  d1.embedding = {3.0f, 4.0f};
  d1.index = 1;
  resp.data.push_back(std::move(d1));

  nlohmann::json j = resp;

  ASSERT_EQ(j["data"].size(), 2u);
  EXPECT_EQ(j["data"][0]["index"], 0);
  EXPECT_EQ(j["data"][1]["index"], 1);
  EXPECT_FLOAT_EQ(j["data"][0]["embedding"][0].get<float>(), 1.0f);
  EXPECT_FLOAT_EQ(j["data"][1]["embedding"][1].get<float>(), 4.0f);
}
