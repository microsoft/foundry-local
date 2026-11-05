// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// SDK and web-service integration tests for the EmbeddingsSession and the
// /v1/embeddings HTTP endpoint. Uses the embeddings model loaded by
// SharedTestEnv.

#include "web_service_fixture.h"

#include "embeddings.h"  // fl::EmbeddingCreateResponse — production header from src/contracts/

#include <cmath>
#include <chrono>
#include <future>

namespace {

// A small set of distinct, semantically varied inputs used to exercise the
// batched and single-input embeddings paths.
const std::vector<std::string>& EmbeddingInputs() {
  static const std::vector<std::string> inputs = {
      "The quick brown fox jumps over the lazy dog.",
      "Foundry Local runs language models on-device.",
      "Embeddings encode text into a vector space.",
  };
  return inputs;
}

void ExpectL2Normalized(const std::vector<float>& v) {
  ASSERT_FALSE(v.empty()) << "Embedding vector should not be empty";

  double sum_sq = 0.0;
  for (float x : v) {
    sum_sq += static_cast<double>(x) * static_cast<double>(x);
  }
  double norm = std::sqrt(sum_sq);

  // EmbeddingsSession L2-normalizes the output. Allow a small tolerance for
  // fp16 rounding in the model and the post-normalization step.
  EXPECT_NEAR(norm, 1.0, 1e-3) << "Expected unit-norm embedding, got |v|=" << norm;
}

double DotProduct(const std::vector<float>& a, const std::vector<float>& b) {
  double dot = 0.0;
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
  }
  return dot;
}

}  // namespace

// ========================================================================
// EmbeddingsFixture — skips when no embeddings model is available.
// ========================================================================

class EmbeddingsFixture : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    SharedTestEnv::Get().AcquireModels({SharedTestEnv::Modality::Embeddings});
  }

  void SetUp() override {
    auto& env = SharedTestEnv::Get();
    if (!env.embeddings_model()) {
      GTEST_SKIP() << "No text-embedding model available";
    }
  }

  static foundry_local::IModel& embeddings_model() {
    return *SharedTestEnv::Get().embeddings_model();
  }

  static const std::string& embeddings_model_id() {
    return SharedTestEnv::Get().embeddings_model_id();
  }
};

// ------------------------------------------------------------------------
// Single-input convenience helper produces an L2-normalized vector.
// ------------------------------------------------------------------------
TEST_F(EmbeddingsFixture, EmbedSingleProducesNormalizedVector) {
  foundry_local::EmbeddingsSession session(embeddings_model());

  auto vec = session.Embed(EmbeddingInputs()[0]);

  ExpectL2Normalized(vec);
}

// ------------------------------------------------------------------------
// Batched embeddings produce one vector per input, all L2-normalized,
// and embeddings for distinct inputs differ.
// ------------------------------------------------------------------------
TEST_F(EmbeddingsFixture, EmbedBatchProducesOnePerInput) {
  foundry_local::EmbeddingsSession session(embeddings_model());

  auto vectors = session.Embed(EmbeddingInputs());

  ASSERT_EQ(vectors.size(), EmbeddingInputs().size());

  size_t expected_dim = vectors[0].size();
  ASSERT_GT(expected_dim, 0u);

  for (const auto& v : vectors) {
    EXPECT_EQ(v.size(), expected_dim) << "All embeddings should share the same dimension";
    ExpectL2Normalized(v);
  }

  // Distinct inputs should produce distinct embeddings (cosine similarity < 1
  // by a noticeable margin).
  double sim_01 = DotProduct(vectors[0], vectors[1]);
  double sim_02 = DotProduct(vectors[0], vectors[2]);
  EXPECT_LT(sim_01, 0.999) << "Distinct inputs should not produce identical embeddings";
  EXPECT_LT(sim_02, 0.999) << "Distinct inputs should not produce identical embeddings";

  // And the same input compared with itself via a batch-of-one should give 1.0.
  auto self = session.Embed(std::vector<std::string>{EmbeddingInputs()[0]});
  ASSERT_EQ(self.size(), 1u);
  EXPECT_NEAR(DotProduct(self[0], vectors[0]), 1.0, 1e-3);
}

// ------------------------------------------------------------------------
// Bit-for-bit equivalence between batched and single-input inference.
// This is the key correctness guarantee of the batching optimization in
// EmbeddingsSession::GenerateEmbeddingsBatch — under causal attention with
// right-padding, the EOS token only attends to real tokens, so the
// per-row outputs from the batched forward pass must match running each
// input independently.
// ------------------------------------------------------------------------
TEST_F(EmbeddingsFixture, BatchedEquivalentToSingleInputs) {
  foundry_local::EmbeddingsSession session(embeddings_model());

  const auto& inputs = EmbeddingInputs();

  // Batched call.
  auto batched = session.Embed(inputs);
  ASSERT_EQ(batched.size(), inputs.size());

  // One-at-a-time calls.
  std::vector<std::vector<float>> singles;
  singles.reserve(inputs.size());
  for (const auto& s : inputs) {
    singles.push_back(session.Embed(s));
  }

  ASSERT_EQ(singles.size(), batched.size());
  for (size_t i = 0; i < batched.size(); ++i) {
    ASSERT_EQ(batched[i].size(), singles[i].size())
        << "row " << i << ": dim mismatch";

    // Allow a tiny tolerance for any non-determinism in the underlying
    // kernels. In practice the values should be identical or within fp16
    // rounding noise.
    for (size_t j = 0; j < batched[i].size(); ++j) {
      ASSERT_NEAR(batched[i][j], singles[i][j], 1e-3)
          << "row " << i << ", dim " << j;
    }
  }
}

TEST_F(EmbeddingsFixture, BatchTokenUsageEqualsSumOfSingleInputs) {
  using namespace foundry_local;

  EmbeddingsSession session(embeddings_model());
  Request batch;
  const std::vector<std::string> inputs = {"hello", EmbeddingInputs()[0]};
  std::vector<int64_t> single_prompt_tokens;
  int64_t prompt_tokens = 0;
  for (const auto& input : inputs) {
    Request single;
    single.AddItem(Item::Text(input));
    auto response = session.ProcessRequest(single);
    auto usage = response.GetUsage();
    EXPECT_GT(usage.prompt_tokens, 0);
    EXPECT_EQ(usage.completion_tokens, 0);
    EXPECT_EQ(usage.total_tokens, usage.prompt_tokens);
    single_prompt_tokens.push_back(usage.prompt_tokens);
    prompt_tokens += usage.prompt_tokens;
    batch.AddItem(Item::Text(input));
  }

  auto response = session.ProcessRequest(batch);
  EXPECT_GT(single_prompt_tokens[1], single_prompt_tokens[0]);
  EXPECT_EQ(response.GetUsage().prompt_tokens, prompt_tokens);
  EXPECT_EQ(response.GetUsage().completion_tokens, 0);
  EXPECT_EQ(response.GetUsage().total_tokens, prompt_tokens);

  Request empty;
  auto empty_response = session.ProcessRequest(empty);
  EXPECT_EQ(empty_response.GetUsage().prompt_tokens, 0);
  EXPECT_EQ(empty_response.GetUsage().total_tokens, 0);
}

TEST_F(EmbeddingsFixture, CancelBatchPreservesConcurrentRequestAndAllowsUnload) {
  using namespace foundry_local;

  {
    EmbeddingsSession session(embeddings_model());
    Request reference;
    reference.AddItem(Item::Text(EmbeddingInputs()[1]));
    const auto expected_usage = session.ProcessRequest(reference).GetUsage();

    Request batch;
    for (size_t i = 0; i < 512; ++i) {
      batch.AddItem(Item::Text(EmbeddingInputs()[0]));
    }

    auto canceled = std::async(std::launch::async, [&] {
      try {
        session.ProcessRequest(batch);
        return FOUNDRY_LOCAL_OK;
      } catch (const Error& error) {
        return error.Code();
      }
    });

    EXPECT_EQ(canceled.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout)
        << "The batch must still be active when cancellation starts";

    auto concurrent = std::async(std::launch::async, [&] {
      Request request;
      request.AddItem(Item::Text(EmbeddingInputs()[1]));
      return session.ProcessRequest(request);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (canceled.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
      // Cancel can race native admission; retry until this request observes cancellation.
      batch.Cancel();
    }

    EXPECT_EQ(canceled.wait_for(std::chrono::milliseconds(0)), std::future_status::ready);
    EXPECT_EQ(canceled.get(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);

    auto response = concurrent.get();
    EXPECT_EQ(response.GetFinishReason(), FOUNDRY_LOCAL_FINISH_STOP);
    ASSERT_EQ(response.GetItems().size(), 1u);
    EXPECT_GT(response.GetUsage().prompt_tokens, 0);
    EXPECT_EQ(response.GetUsage().prompt_tokens, expected_usage.prompt_tokens);
    EXPECT_EQ(response.GetUsage().total_tokens, expected_usage.total_tokens);

    auto reused = session.Embed(EmbeddingInputs()[2]);
    ExpectL2Normalized(reused);
  }

  embeddings_model().Unload();
  EXPECT_FALSE(embeddings_model().IsLoaded());
  embeddings_model().Load();
  EXPECT_TRUE(embeddings_model().IsLoaded());
}

// ========================================================================
// EmbeddingsWebServiceFixture — exercises POST /v1/embeddings.
// ========================================================================

class EmbeddingsWebServiceFixture : public WebServiceFixture {
 protected:
  static void SetUpTestSuite() {
    SharedTestEnv::Get().AcquireModels({SharedTestEnv::Modality::Embeddings});
  }

  void SetUp() override {
    auto& env = SharedTestEnv::Get();
    if (!env.embeddings_model()) {
      GTEST_SKIP() << "No text-embedding model available";
    }
  }

  static const std::string& embeddings_model_id() {
    return SharedTestEnv::Get().embeddings_model_id();
  }
};

// ------------------------------------------------------------------------
// /v1/embeddings with a single string input returns one embedding.
// ------------------------------------------------------------------------
TEST_F(EmbeddingsWebServiceFixture, PostSingleStringInput) {
  auto client = MakeClient();

  json request_body = {
      {"model", embeddings_model_id()},
      {"input", EmbeddingInputs()[0]},
  };

  auto result = client.Post("/v1/embeddings", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  ASSERT_EQ(response.value("object", ""), "list") << response.dump(2);
  ASSERT_EQ(response.value("model", ""), embeddings_model_id());
  ASSERT_TRUE(response.contains("data")) << response.dump(2);
  ASSERT_EQ(response["data"].size(), 1u);

  const auto& entry = response["data"][0];
  EXPECT_EQ(entry.value("object", ""), "embedding");
  EXPECT_EQ(entry.value("index", -1), 0);
  ASSERT_TRUE(entry.contains("embedding"));

  auto vec = entry["embedding"].get<std::vector<float>>();
  ExpectL2Normalized(vec);
  EXPECT_GT(response.at("usage").at("prompt_tokens").get<int64_t>(), 0);
  EXPECT_EQ(response["usage"]["prompt_tokens"], response["usage"]["total_tokens"]);
}

// ------------------------------------------------------------------------
// /v1/embeddings with an array of inputs returns one entry per input,
// each at the correct index. The HTTP response embeddings should match
// the ones produced by the in-process EmbeddingsSession.
// ------------------------------------------------------------------------
TEST_F(EmbeddingsWebServiceFixture, PostArrayInputMatchesInProcess) {
  auto client = MakeClient();

  json request_body = {
      {"model", embeddings_model_id()},
      {"input", EmbeddingInputs()},
  };

  auto result = client.Post("/v1/embeddings", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  ASSERT_EQ(result->status, 200) << result->body;

  json response = json::parse(result->body);
  ASSERT_TRUE(response.contains("data")) << response.dump(2);
  ASSERT_EQ(response["data"].size(), EmbeddingInputs().size());

  std::vector<std::vector<float>> http_vectors;
  http_vectors.reserve(EmbeddingInputs().size());
  for (size_t i = 0; i < response["data"].size(); ++i) {
    const auto& entry = response["data"][i];
    EXPECT_EQ(entry.value("index", -1), static_cast<int>(i));

    auto v = entry["embedding"].get<std::vector<float>>();
    ExpectL2Normalized(v);
    http_vectors.push_back(std::move(v));
  }

  // Compare against the in-process session for the same batch.
  foundry_local::EmbeddingsSession session(
      *SharedTestEnv::Get().embeddings_model());
  auto direct = session.Embed(EmbeddingInputs());

  foundry_local::Request typed_request;
  for (const auto& input : EmbeddingInputs()) {
    typed_request.AddItem(foundry_local::Item::Text(input));
  }

  auto typed_response = session.ProcessRequest(typed_request);
  EXPECT_GT(typed_response.GetUsage().prompt_tokens, 0);
  EXPECT_EQ(response.at("usage").at("prompt_tokens"), typed_response.GetUsage().prompt_tokens);
  EXPECT_EQ(response.at("usage").at("total_tokens"), typed_response.GetUsage().total_tokens);

  ASSERT_EQ(direct.size(), http_vectors.size());
  for (size_t i = 0; i < direct.size(); ++i) {
    ASSERT_EQ(direct[i].size(), http_vectors[i].size()) << "row " << i;
    for (size_t j = 0; j < direct[i].size(); ++j) {
      ASSERT_NEAR(direct[i][j], http_vectors[i][j], 1e-3)
          << "row " << i << ", dim " << j;
    }
  }
}

// ------------------------------------------------------------------------
// Empty input array is rejected with HTTP 400.
// ------------------------------------------------------------------------
TEST_F(EmbeddingsWebServiceFixture, PostEmptyInputReturns400) {
  auto client = MakeClient();

  json request_body = {
      {"model", embeddings_model_id()},
      {"input", json::array()},
  };

  auto result = client.Post("/v1/embeddings", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  EXPECT_EQ(result->status, 400) << result->body;
}

// ------------------------------------------------------------------------
// Unknown model is rejected with HTTP 404.
// ------------------------------------------------------------------------
TEST_F(EmbeddingsWebServiceFixture, PostUnknownModelReturns404) {
  auto client = MakeClient();

  json request_body = {
      {"model", "this-model-does-not-exist:1"},
      {"input", EmbeddingInputs()[0]},
  };

  auto result = client.Post("/v1/embeddings", request_body.dump(), "application/json");
  ASSERT_TRUE(result) << "HTTP request failed: " << httplib::to_string(result.error());
  EXPECT_EQ(result->status, 404) << result->body;
}

// ========================================================================
// OPENAI_JSON pass-through tests on EmbeddingsSession (public C++ API).
//
// The OpenAI-JSON request/response is the contract every other-language SDK
// consumes via the web service. These tests exercise the contract directly
// against the in-process EmbeddingsSession, without going through HTTP, so
// drift is caught at the session layer rather than the HTTP boundary.
// ========================================================================

namespace {

// Inline parser for fl::EmbeddingCreateResponse. The production
// contracts/embeddings.h only provides to_json for the response type
// (the request goes the other direction). Mirrors the
// chat_completions_from_json.cc precedent — test-only deserialization
// next to the test that needs it.
fl::EmbeddingCreateResponse ParseEmbeddingResponse(const std::string& body) {
  auto j = nlohmann::json::parse(body);

  fl::EmbeddingCreateResponse out;
  out.object = j.at("object").get<std::string>();
  out.model = j.at("model").get<std::string>();

  for (const auto& entry : j.at("data")) {
    fl::EmbeddingData d;
    d.object = entry.at("object").get<std::string>();
    d.embedding = entry.at("embedding").get<std::vector<float>>();
    d.index = entry.at("index").get<int>();
    out.data.push_back(std::move(d));
  }

  if (j.contains("usage")) {
    const auto& u = j.at("usage");
    out.usage.prompt_tokens = u.value("prompt_tokens", int64_t{0});
    out.usage.total_tokens = u.value("total_tokens", int64_t{0});
  }

  return out;
}

// Run a request through EmbeddingsSession via the public API and return the
// single OPENAI_JSON-tagged TextItem payload. Asserts the response shape
// (one item, TEXT type, OPENAI_JSON subtype).
std::string RunOpenAIJsonRequest(foundry_local::EmbeddingsSession& session,
                                 const std::string& request_json) {
  using namespace foundry_local;

  Request request;
  request.AddItem(Item::Text(request_json, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));

  Response response = session.ProcessRequest(request);

  EXPECT_EQ(response.GetFinishReason(), FOUNDRY_LOCAL_FINISH_STOP);

  const auto& items = response.GetItems();
  EXPECT_EQ(items.size(), 1u) << "OpenAI JSON path must produce exactly one response item";

  if (items.size() != 1) {
    return {};
  }

  EXPECT_EQ(items[0].GetType(), FOUNDRY_LOCAL_ITEM_TEXT)
      << "Expected TEXT item, got type " << static_cast<int>(items[0].GetType());

  auto tc = items[0].GetText();
  EXPECT_EQ(tc.type, FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON)
      << "Expected OPENAI_JSON subtype, got " << static_cast<int>(tc.type);

  auto payload = nlohmann::json::parse(tc.text);
  EXPECT_EQ(payload.at("usage").at("prompt_tokens"), response.GetUsage().prompt_tokens);
  EXPECT_EQ(payload.at("usage").at("total_tokens"), response.GetUsage().total_tokens);
  EXPECT_EQ(response.GetUsage().completion_tokens, 0);

  return std::string(tc.text);
}

}  // namespace

// ------------------------------------------------------------------------
// Single-string "input" produces one EmbeddingData entry, L2-normalized.
// ------------------------------------------------------------------------
TEST_F(EmbeddingsFixture, OpenAIJsonSingleStringInput) {
  foundry_local::EmbeddingsSession session(embeddings_model());

  json req = {
      {"model", embeddings_model_id()},
      {"input", "The quick brown fox jumps over the lazy dog."},
  };

  std::string response_text = RunOpenAIJsonRequest(session, req.dump());
  ASSERT_FALSE(response_text.empty());

  fl::EmbeddingCreateResponse output = ParseEmbeddingResponse(response_text);

  EXPECT_EQ(output.object, "list");
  EXPECT_EQ(output.model, embeddings_model_id());

  ASSERT_EQ(output.data.size(), 1u);
  EXPECT_EQ(output.data[0].object, "embedding");
  EXPECT_EQ(output.data[0].index, 0);
  EXPECT_GT(output.data[0].embedding.size(), 0u);

  ExpectL2Normalized(output.data[0].embedding);
}

// ------------------------------------------------------------------------
// Array "input" produces one EmbeddingData per entry, indices in order,
// all L2-normalized and same dimensionality.
// ------------------------------------------------------------------------
TEST_F(EmbeddingsFixture, OpenAIJsonArrayInput) {
  foundry_local::EmbeddingsSession session(embeddings_model());

  json req = {
      {"model", embeddings_model_id()},
      {"input", EmbeddingInputs()},
  };

  std::string response_text = RunOpenAIJsonRequest(session, req.dump());
  ASSERT_FALSE(response_text.empty());

  fl::EmbeddingCreateResponse output = ParseEmbeddingResponse(response_text);

  EXPECT_EQ(output.object, "list");
  EXPECT_EQ(output.model, embeddings_model_id());
  ASSERT_EQ(output.data.size(), EmbeddingInputs().size());

  EXPECT_EQ(output.data[0].index, 0);
  EXPECT_EQ(output.data[1].index, 1);
  EXPECT_EQ(output.data[2].index, 2);

  size_t dim = output.data[0].embedding.size();
  ASSERT_GT(dim, 0u);

  for (const auto& d : output.data) {
    EXPECT_EQ(d.object, "embedding");
    EXPECT_EQ(d.embedding.size(), dim) << "All embeddings should share dimension";
    ExpectL2Normalized(d.embedding);
  }
}

// ------------------------------------------------------------------------
// Regression guard: typed (Item::Text → TensorItem) and JSON paths share
// GenerateEmbeddingsBatch internally, so element-by-element float values
// must match exactly. If a future refactor splits the two paths, this
// test catches it before the contract drifts on the wire.
// ------------------------------------------------------------------------
TEST_F(EmbeddingsFixture, OpenAIJsonResponseParityWithTypedPath) {
  using namespace foundry_local;

  EmbeddingsSession session(embeddings_model());

  const std::string& s = EmbeddingInputs()[0];

  // Typed path: a TEXT item with DEFAULT subtype goes through ProcessRequestImpl's
  // TENSOR-producing branch.
  Request typed_request;
  typed_request.AddItem(Item::Text(s));

  Response typed_response = session.ProcessRequest(typed_request);

  ASSERT_FALSE(typed_response.GetItems().empty());

  std::vector<float> typed_vec;
  for (const auto& item : typed_response.GetItems()) {
    if (item.GetType() == FOUNDRY_LOCAL_ITEM_TENSOR) {
      auto tc = item.GetTensor();
      ASSERT_EQ(tc.data_type, FOUNDRY_LOCAL_TENSOR_FLOAT);

      size_t count = 1;
      for (auto d : tc.shape) {
        count *= static_cast<size_t>(d);
      }

      const float* data = static_cast<const float*>(tc.data);
      typed_vec.assign(data, data + count);
      break;
    }
  }

  ASSERT_FALSE(typed_vec.empty()) << "Typed path produced no TENSOR item";

  // JSON path.
  json req = {
      {"model", embeddings_model_id()},
      {"input", s},
  };

  std::string response_text = RunOpenAIJsonRequest(session, req.dump());
  ASSERT_FALSE(response_text.empty());

  fl::EmbeddingCreateResponse output = ParseEmbeddingResponse(response_text);
  ASSERT_EQ(output.data.size(), 1u);
  EXPECT_GT(typed_response.GetUsage().prompt_tokens, 0);
  EXPECT_EQ(output.usage.prompt_tokens, typed_response.GetUsage().prompt_tokens);
  EXPECT_EQ(output.usage.total_tokens, typed_response.GetUsage().total_tokens);

  const auto& json_vec = output.data[0].embedding;

  ASSERT_EQ(json_vec.size(), typed_vec.size())
      << "Typed and JSON paths must produce same-sized vectors";

  for (size_t i = 0; i < typed_vec.size(); ++i) {
    EXPECT_FLOAT_EQ(typed_vec[i], json_vec[i])
        << "Typed/JSON parity violated at dim " << i;
  }
}

// ------------------------------------------------------------------------
// Malformed JSON in the OPENAI_JSON TextItem body propagates through the
// public-API boundary as a foundry_local::Error (the C ABI wraps all
// exceptions, including nlohmann's parse_error, but preserves the message).
// ------------------------------------------------------------------------
TEST_F(EmbeddingsFixture, OpenAIJsonInvalidJsonThrows) {
  using namespace foundry_local;

  EmbeddingsSession session(embeddings_model());

  Request request;
  request.AddItem(Item::Text("not valid json {{{", FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));

  try {
    session.ProcessRequest(request);
    FAIL() << "Expected ProcessRequest to throw on malformed JSON";
  } catch (const Error& e) {
    EXPECT_NE(std::string(e.what()).find("parse_error"), std::string::npos)
        << "Expected nlohmann parse_error in message: " << e.what();
  }
}

// ------------------------------------------------------------------------
// Missing "input" key throws. nlohmann's json::out_of_range is wrapped at
// the public-API boundary as foundry_local::Error; the message preserves
// the original "key 'input' not found" text.
// ------------------------------------------------------------------------
TEST_F(EmbeddingsFixture, OpenAIJsonMissingInputFieldThrows) {
  using namespace foundry_local;

  EmbeddingsSession session(embeddings_model());

  json req = {
      {"model", embeddings_model_id()},
  };

  Request request;
  request.AddItem(Item::Text(req.dump(), FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON));

  try {
    session.ProcessRequest(request);
    FAIL() << "Expected ProcessRequest to throw when 'input' is missing";
  } catch (const Error& e) {
    EXPECT_NE(std::string(e.what()).find("input"), std::string::npos)
        << "Expected message to mention 'input': " << e.what();
  }
}
