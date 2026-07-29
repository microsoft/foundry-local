// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for the oatpp web service handlers.

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE

#include "catalog.h"
#include "ep_detection/ep_detector.h"
#include "http/http_client.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/session/session_manager.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"
#include "internal_api/web_service_test_helpers.h"
#include "logger.h"
#include "model.h"
#include "model_info.h"
#include "null_telemetry.h"
#include "service/web_service.h"

#include <foundry_local/foundry_local_c.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <httplib.h>

using namespace fl;
using json = nlohmann::json;

namespace {

std::string TestHttpGet(const std::string& url, const std::string& user_agent = "") {
  http::HttpRequestOptions options;
  options.user_agent = user_agent;
  options.close_connection = true;
  return http::HttpGet(url, options);
}

std::string TestHttpPost(const std::string& url, const std::string& json_body,
                         const std::string& user_agent = "") {
  http::HttpRequestOptions options;
  options.user_agent = user_agent;
  options.close_connection = true;
  return http::HttpPost(url, json_body, options);
}

std::string TestHttpDelete(const std::string& url, const std::string& user_agent = "") {
  http::HttpRequestOptions options;
  options.user_agent = user_agent;
  options.close_connection = true;
  return http::HttpDelete(url, options);
}

}  // namespace

// ========================================================================
// Test fixture — starts a real web service on an ephemeral port
// ========================================================================

class WebServiceTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    logger_ = std::make_unique<StderrLogger>();
    ep_detector_ = std::make_unique<test::CpuOnlyEpDetector>();
    model_load_manager_ = std::make_unique<ModelLoadManager>(*ep_detector_, *logger_);
    session_manager_ = std::make_unique<SessionManager>(*logger_);
    null_telemetry_ = std::make_unique<fl::test::NullTelemetry>();
    catalog_ = std::make_unique<test::MockCatalog>();

    // Populate with test models
    catalog_->AddModel(Model::FromModelInfo(
        test::MakeTestModelInfo("alpha-model", "acme-corp"), "",
        svc_.download_manager, svc_.model_load_manager));
    catalog_->AddModel(Model::FromModelInfo(
        test::MakeTestModelInfo("beta-model", "contoso"), "",
        svc_.download_manager, svc_.model_load_manager));

    const auto loadable_model_path = test::GetTestDataModelPath(test::kLoadableTestModelAlias);
    ASSERT_TRUE(std::filesystem::exists(loadable_model_path))
        << "Expected loadable test model at " << loadable_model_path;
    catalog_->AddModel(Model::FromModelInfo(
        test::MakeTestModelInfo(test::kLoadableTestModelAlias, "microsoft"),
        loadable_model_path,
        svc_.download_manager,
        *model_load_manager_));

    service_ = std::make_unique<WebService>(*catalog_, *logger_, "/tmp/test-cache",
                                            *model_load_manager_, *session_manager_, *null_telemetry_, []() {});
    auto urls = service_->Start({"http://127.0.0.1:0"});
    ASSERT_EQ(urls.size(), 1u);
    base_url_ = urls[0];
  }

  static void TearDownTestSuite() {
    if (service_) {
      service_->Stop();
    }
    service_.reset();
    catalog_.reset();
    session_manager_.reset();
    null_telemetry_.reset();
    model_load_manager_.reset();
    ep_detector_.reset();
    logger_.reset();
  }

  // Convenience: GET a path and parse the response as JSON.
  json Get(const std::string& path) {
    auto body = TestHttpGet(base_url_ + path);
    return json::parse(body);
  }

  static std::unique_ptr<test::MockCatalog> catalog_;
  static std::unique_ptr<test::CpuOnlyEpDetector> ep_detector_;
  static std::unique_ptr<StderrLogger> logger_;
  static std::unique_ptr<ModelLoadManager> model_load_manager_;
  static std::unique_ptr<SessionManager> session_manager_;
  static std::unique_ptr<fl::test::NullTelemetry> null_telemetry_;
  static std::unique_ptr<WebService> service_;
  static std::string base_url_;
  static inline fl::test::FakeServiceBindings svc_;
};

// Static member definitions
std::unique_ptr<test::MockCatalog> WebServiceTest::catalog_;
std::unique_ptr<test::CpuOnlyEpDetector> WebServiceTest::ep_detector_;
std::unique_ptr<StderrLogger> WebServiceTest::logger_;
std::unique_ptr<ModelLoadManager> WebServiceTest::model_load_manager_;
std::unique_ptr<SessionManager> WebServiceTest::session_manager_;
std::unique_ptr<fl::test::NullTelemetry> WebServiceTest::null_telemetry_;
std::unique_ptr<WebService> WebServiceTest::service_;
std::string WebServiceTest::base_url_;

// ========================================================================
// GET /status
// ========================================================================

TEST_F(WebServiceTest, StatusReturnsModelCachePath) {
  auto j = Get("/status");

  EXPECT_EQ(j["modelCachePath"], "/tmp/test-cache")
      << "Response: " << j.dump(2);
}

TEST_F(WebServiceTest, StatusReturnsEndpoints) {
  auto j = Get("/status");

  ASSERT_TRUE(j.contains("endpoints")) << "Response: " << j.dump(2);
  ASSERT_TRUE(j["endpoints"].is_array()) << "Response: " << j.dump(2);
  EXPECT_GE(j["endpoints"].size(), 1u)
      << "Expected at least one endpoint. Response: " << j.dump(2);

  // The endpoint should match our base_url_
  bool found = false;
  for (const auto& ep : j["endpoints"]) {
    if (ep.get<std::string>() == base_url_) {
      found = true;
      break;
    }
  }

  EXPECT_TRUE(found) << "Expected base_url '" << base_url_
                     << "' in endpoints. Response: " << j.dump(2);
}

// ========================================================================
// GET /models/loaded
// ========================================================================

TEST_F(WebServiceTest, LoadedModelsReturnsEmptyWhenNoneLoaded) {
  auto j = Get("/models/loaded");

  ASSERT_TRUE(j.is_array()) << "Response: " << j.dump(2);
  EXPECT_EQ(j.size(), 0u) << "Expected empty array. Response: " << j.dump(2);
}

TEST_F(WebServiceTest, LoadedModelsReturnsModelIds) {
  auto* model = catalog_->GetModel(test::kLoadableTestModelAlias);
  ASSERT_NE(model, nullptr);
  ASSERT_TRUE(model->IsCached());

  auto load_result = Get(std::string("/models/load/") + test::kLoadableTestModelAlias);
  ASSERT_EQ(load_result["status"], "loaded") << "Response: " << load_result.dump(2);

  auto j = Get("/models/loaded");

  ASSERT_TRUE(j.is_array()) << "Response: " << j.dump(2);
  EXPECT_EQ(j.size(), 1u) << "Response: " << j.dump(2);
  EXPECT_EQ(j[0].get<std::string>(), std::string(test::kLoadableTestModelAlias) + ":1")
      << "Response: " << j.dump(2);
}

// ========================================================================
// GET /models/load/{name}
// ========================================================================

TEST_F(WebServiceTest, LoadModelReturnsNotFoundForUnknownModel) {
  // "nonexistent" is not in the catalog
  EXPECT_THROW(TestHttpGet(base_url_ + "/models/load/nonexistent"),
               std::exception);
}

TEST_F(WebServiceTest, LoadModelReturnsBadRequestWhenNotCached) {
  // "alpha-model" exists but is not cached (IsCached() == false)
  // The handler should return 400 or a non-success status.
  // WinHTTP throws on non-2xx responses.
  EXPECT_THROW(TestHttpGet(base_url_ + "/models/load/alpha-model"),
               std::exception);
}

// ========================================================================
// GET /models/unload/{name}
// ========================================================================

TEST_F(WebServiceTest, UnloadModelReturnsNotFoundForUnknownModel) {
  EXPECT_THROW(TestHttpGet(base_url_ + "/models/unload/nonexistent"),
               std::exception);
}

TEST_F(WebServiceTest, UnloadModelReturnsNotLoadedStatus) {
  // Model exists but is not loaded — handler should return 200 with "not_loaded"
  auto j = Get("/models/unload/alpha-model");

  EXPECT_EQ(j["status"], "not_loaded") << "Response: " << j.dump(2);
}

TEST_F(WebServiceTest, UnloadModelReturnsUnloadedStatusForLoadedModel) {
  auto* model = catalog_->GetModel(test::kLoadableTestModelAlias);
  ASSERT_NE(model, nullptr);
  ASSERT_TRUE(model->IsCached());

  auto load_result = Get(std::string("/models/load/") + test::kLoadableTestModelAlias);
  auto status = load_result["status"].get<std::string>();
  ASSERT_TRUE(status == "loaded" || status == "already_loaded") << "Response: " << load_result.dump(2);
  ASSERT_TRUE(model->IsLoaded());

  auto j = Get(std::string("/models/unload/") + test::kLoadableTestModelAlias);

  EXPECT_EQ(j["status"], "unloaded") << "Response: " << j.dump(2);
  EXPECT_FALSE(model->IsLoaded());
}

// ========================================================================
// GET /v1/models — OpenAI-compatible
// ========================================================================

TEST_F(WebServiceTest, OpenAIListModelsReturnsAllModels) {
  auto j = Get("/v1/models");

  EXPECT_EQ(j["object"], "list") << "Response: " << j.dump(2);
  ASSERT_TRUE(j["data"].is_array()) << "Response: " << j.dump(2);
  EXPECT_EQ(j["data"].size(), 3u) << "Expected 3 models. Response: " << j.dump(2);
}

TEST_F(WebServiceTest, OpenAIListModelsContainsExpectedFields) {
  auto j = Get("/v1/models");
  const auto& first = j["data"][0];

  EXPECT_TRUE(first.contains("id")) << "Missing 'id'. Response: " << first.dump(2);
  EXPECT_TRUE(first.contains("object")) << "Missing 'object'. Response: " << first.dump(2);
  EXPECT_TRUE(first.contains("created")) << "Missing 'created'. Response: " << first.dump(2);
  EXPECT_TRUE(first.contains("owned_by")) << "Missing 'owned_by'. Response: " << first.dump(2);

  EXPECT_EQ(first["object"], "model") << "Response: " << first.dump(2);
}

TEST_F(WebServiceTest, OpenAIListModelsPopulatesPublisher) {
  auto j = Get("/v1/models");

  // Find alpha-model:1 (model_id) and verify publisher
  bool found = false;
  for (const auto& m : j["data"]) {
    if (m["id"] == "alpha-model:1") {
      EXPECT_EQ(m["owned_by"], "acme-corp") << "Response: " << m.dump(2);
      EXPECT_EQ(m["created"], 1700000000) << "Response: " << m.dump(2);
      found = true;
      break;
    }
  }

  EXPECT_TRUE(found) << "alpha-model:1 not found in response: " << j.dump(2);
}

// ========================================================================
// GET /v1/models/{name} — OpenAI-compatible retrieve
// ========================================================================

TEST_F(WebServiceTest, OpenAIRetrieveModelReturnsModelInfo) {
  auto j = Get("/v1/models/alpha-model:1");

  EXPECT_EQ(j["id"], "alpha-model:1") << "Response: " << j.dump(2);
  EXPECT_EQ(j["object"], "model") << "Response: " << j.dump(2);
  EXPECT_EQ(j["owned_by"], "acme-corp") << "Response: " << j.dump(2);
  EXPECT_EQ(j["created"], 1700000000) << "Response: " << j.dump(2);
}

TEST_F(WebServiceTest, OpenAIRetrieveModelReturnsNotFoundForUnknownModel) {
  EXPECT_THROW(TestHttpGet(base_url_ + "/v1/models/nonexistent"),
               std::exception);
}

TEST_F(WebServiceTest, OpenAIRetrieveSecondModel) {
  auto j = Get("/v1/models/beta-model:1");

  EXPECT_EQ(j["id"], "beta-model:1") << "Response: " << j.dump(2);
  EXPECT_EQ(j["owned_by"], "contoso") << "Response: " << j.dump(2);
}

// ========================================================================
// POST /v1/chat/completions — validation & stub
// ========================================================================

TEST_F(WebServiceTest, ChatCompletionsRejectsEmptyBody) {
  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/chat/completions", ""),
               std::exception);
}

TEST_F(WebServiceTest, ChatCompletionsRejectsInvalidJson) {
  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/chat/completions", "not json"),
               std::exception);
}

TEST_F(WebServiceTest, ChatCompletionsRejectsMissingModel) {
  json body = {
      {"messages", json::array({{{"role", "user"}, {"content", "hello"}}})}};

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/chat/completions", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, ChatCompletionsRejectsMissingMessages) {
  json body = {{"model", "alpha-model"}};

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/chat/completions", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, ChatCompletionsRejectsUnknownModel) {
  json body = {
      {"model", "nonexistent-model"},
      {"messages", json::array({{{"role", "user"}, {"content", "hello"}}})},
  };

  // 404 for unknown model
  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/chat/completions", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, ChatCompletionsRejectsInvalidMessage) {
  // Message missing "content" field
  json body = {
      {"model", "alpha-model"},
      {"messages", json::array({{{"role", "user"}}})},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/chat/completions", body.dump()),
               std::exception);
}

// ========================================================================
// WebService lifecycle tests
// ========================================================================

TEST(WebServiceLifecycleTest, StartAndStopOnEphemeralPort) {
  test::MockCatalog catalog;
  StderrLogger logger;
  test::CpuOnlyEpDetector ep_detector;
  ModelLoadManager model_load_manager(ep_detector, logger);
  SessionManager session_manager(logger);
  fl::test::NullTelemetry null_telemetry;

  WebService service(catalog, logger, "/tmp/test", model_load_manager, session_manager, null_telemetry, []() {});
  auto urls = service.Start({"http://127.0.0.1:0"});

  ASSERT_EQ(urls.size(), 1u);
  EXPECT_NE(urls[0].find("http://127.0.0.1:"), std::string::npos)
      << "Bound URL: " << urls[0];

  // Port should not be 0 (it should have been resolved)
  EXPECT_EQ(urls[0].find(":0"), std::string::npos)
      << "Port was not resolved. URL: " << urls[0];

  service.Stop();
}

TEST(WebServiceLifecycleTest, DoubleStartThrows) {
  test::MockCatalog catalog;
  StderrLogger logger;
  test::CpuOnlyEpDetector ep_detector;
  ModelLoadManager model_load_manager(ep_detector, logger);
  SessionManager session_manager(logger);
  fl::test::NullTelemetry null_telemetry;

  WebService service(catalog, logger, "/tmp/test", model_load_manager, session_manager, null_telemetry, []() {});
  service.Start({"http://127.0.0.1:0"});

  EXPECT_THROW(service.Start({"http://127.0.0.1:0"}), std::runtime_error);

  service.Stop();
}

TEST(WebServiceLifecycleTest, StopWithoutStartIsNoop) {
  test::MockCatalog catalog;
  StderrLogger logger;
  test::CpuOnlyEpDetector ep_detector;
  ModelLoadManager model_load_manager(ep_detector, logger);
  SessionManager session_manager(logger);
  fl::test::NullTelemetry null_telemetry;

  WebService service(catalog, logger, "/tmp/test", model_load_manager, session_manager, null_telemetry, []() {});
  // Should not crash
  service.Stop();
}

TEST(WebServiceLifecycleTest, MultipleEndpoints) {
  test::MockCatalog catalog;
  StderrLogger logger;
  test::CpuOnlyEpDetector ep_detector;
  ModelLoadManager model_load_manager(ep_detector, logger);
  SessionManager session_manager(logger);
  fl::test::NullTelemetry null_telemetry;

  WebService service(catalog, logger, "/tmp/test", model_load_manager, session_manager, null_telemetry, []() {});
  auto urls = service.Start({"http://127.0.0.1:0", "http://127.0.0.1:0"});

  EXPECT_EQ(urls.size(), 2u) << "Expected 2 bound URLs";

  // Both should have different ports
  if (urls.size() == 2) {
    EXPECT_NE(urls[0], urls[1])
        << "Two endpoints should bind to different ports";
  }

  service.Stop();
}

// ========================================================================
// Empty catalog tests
// ========================================================================

TEST(WebServiceEmptyCatalogTest, ListModelsReturnsEmptyData) {
  test::MockCatalog catalog;  // No models added
  StderrLogger logger;
  test::CpuOnlyEpDetector ep_detector;
  ModelLoadManager model_load_manager(ep_detector, logger);
  SessionManager session_manager(logger);
  fl::test::NullTelemetry null_telemetry;

  WebService service(catalog, logger, "/tmp/test", model_load_manager, session_manager, null_telemetry, []() {});
  auto urls = service.Start({"http://127.0.0.1:0"});

  auto body = TestHttpGet(urls[0] + "/v1/models");
  auto j = json::parse(body);

  EXPECT_EQ(j["object"], "list") << "Response: " << j.dump(2);
  EXPECT_EQ(j["data"].size(), 0u) << "Expected empty list. Response: " << j.dump(2);

  service.Stop();
}

TEST(WebServiceEmptyCatalogTest, LoadedModelsReturnsEmptyArray) {
  test::MockCatalog catalog;
  StderrLogger logger;
  test::CpuOnlyEpDetector ep_detector;
  ModelLoadManager model_load_manager(ep_detector, logger);
  SessionManager session_manager(logger);
  fl::test::NullTelemetry null_telemetry;

  WebService service(catalog, logger, "/tmp/test", model_load_manager, session_manager, null_telemetry, []() {});
  auto urls = service.Start({"http://127.0.0.1:0"});

  auto body = TestHttpGet(urls[0] + "/models/loaded");
  auto j = json::parse(body);

  ASSERT_TRUE(j.is_array()) << "Response: " << j.dump(2);
  EXPECT_EQ(j.size(), 0u) << "Response: " << j.dump(2);

  service.Stop();
}

// ========================================================================
// Streaming validation tests — same errors apply with stream=true
// ========================================================================

TEST_F(WebServiceTest, StreamingRejectsEmptyBody) {
  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/chat/completions", ""),
               std::exception);
}

TEST_F(WebServiceTest, StreamingRejectsMissingModel) {
  json body = {
      {"messages", json::array({{{"role", "user"}, {"content", "hello"}}})},
      {"stream", true},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/chat/completions", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, StreamingRejectsMissingMessages) {
  json body = {
      {"model", "alpha-model"},
      {"stream", true},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/chat/completions", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, StreamingRejectsUnknownModel) {
  json body = {
      {"model", "nonexistent-model"},
      {"messages", json::array({{{"role", "user"}, {"content", "hello"}}})},
      {"stream", true},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/chat/completions", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, StreamingRejectsInvalidMessage) {
  json body = {
      {"model", "alpha-model"},
      {"messages", json::array({{{"role", "user"}}})},
      {"stream", true},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/chat/completions", body.dump()),
               std::exception);
}

// ========================================================================
// POST /v1/responses — Responses API validation
// ========================================================================

TEST_F(WebServiceTest, ResponsesRejectsEmptyBody) {
  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/responses", ""),
               std::exception);
}

TEST_F(WebServiceTest, ResponsesRejectsInvalidJson) {
  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/responses", "not json"),
               std::exception);
}

TEST_F(WebServiceTest, ResponsesRejectsMissingModel) {
  json body = {
      {"input", "hello"},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/responses", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, ResponsesRejectsMissingInput) {
  json body = {
      {"model", "alpha-model"},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/responses", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, ResponsesRejectsUnknownModel) {
  json body = {
      {"model", "nonexistent-model"},
      {"input", "hello"},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/responses", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, ResponsesRejectsInvalidInputType) {
  json body = {
      {"model", "alpha-model"},
      {"input", 42},  // Must be string or array
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/responses", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, ResponsesRejectsInvalidInputItem) {
  json body = {
      {"model", "alpha-model"},
      {"input", json::array({"not an object"})},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/responses", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, ResponsesRejectsInputItemMissingRole) {
  json body = {
      {"model", "alpha-model"},
      {"input", json::array({{{"content", "hello"}}})},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/responses", body.dump()),
               std::exception);
}

// ========================================================================
// POST /v1/responses streaming validation
// ========================================================================

TEST_F(WebServiceTest, ResponsesStreamingRejectsMissingModel) {
  json body = {
      {"input", "hello"},
      {"stream", true},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/responses", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, ResponsesStreamingRejectsMissingInput) {
  json body = {
      {"model", "alpha-model"},
      {"stream", true},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/responses", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, ResponsesStreamingRejectsUnknownModel) {
  json body = {
      {"model", "nonexistent-model"},
      {"input", "hello"},
      {"stream", true},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/responses", body.dump()),
               std::exception);
}

// ========================================================================
// GET /v1/responses/{id} — Retrieve stored response
// ========================================================================

TEST_F(WebServiceTest, GetResponseReturnsNotFoundForMissingId) {
  EXPECT_THROW(TestHttpGet(base_url_ + "/v1/responses/resp_nonexistent"),
               std::exception);
}

// ========================================================================
// GET /v1/responses — List stored responses
// ========================================================================

TEST_F(WebServiceTest, ListResponsesReturnsEmptyList) {
  auto j = Get("/v1/responses");

  EXPECT_EQ(j["object"], "list") << "Response: " << j.dump(2);
  ASSERT_TRUE(j["data"].is_array()) << "Response: " << j.dump(2);
  EXPECT_EQ(j["data"].size(), 0u) << "Response: " << j.dump(2);
  EXPECT_FALSE(j["has_more"].get<bool>()) << "Response: " << j.dump(2);
}

TEST_F(WebServiceTest, ListResponsesRespectsLimitParam) {
  auto j = Get("/v1/responses?limit=5&order=asc");

  EXPECT_EQ(j["object"], "list") << "Response: " << j.dump(2);
  ASSERT_TRUE(j["data"].is_array()) << "Response: " << j.dump(2);
}

// ========================================================================
// DELETE /v1/responses/{id} — Delete stored response
// ========================================================================

TEST_F(WebServiceTest, DeleteResponseReturnsNotFoundForMissingId) {
  EXPECT_THROW(TestHttpDelete(base_url_ + "/v1/responses/resp_nonexistent"),
               std::exception);
}

// ========================================================================
// GET /v1/responses/{id}/input_items — Get input items
// ========================================================================

TEST_F(WebServiceTest, GetInputItemsReturnsNotFoundForMissingId) {
  EXPECT_THROW(
      TestHttpGet(base_url_ + "/v1/responses/resp_nonexistent/input_items"),
      std::exception);
}

// ========================================================================
// POST /v1/audio/transcriptions — validation (no real audio model needed)
// ========================================================================

TEST_F(WebServiceTest, AudioTranscriptionRejectsEmptyBody) {
  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/audio/transcriptions", ""),
               std::exception);
}

TEST_F(WebServiceTest, AudioTranscriptionRejectsInvalidJson) {
  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/audio/transcriptions", "not json"),
               std::exception);
}

TEST_F(WebServiceTest, AudioTranscriptionRejectsMissingModel) {
  json body = {
      {"file", "/some/audio.mp3"},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/audio/transcriptions", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, AudioTranscriptionRejectsMissingFile) {
  json body = {
      {"model", "alpha-model"},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/audio/transcriptions", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, AudioTranscriptionRejectsUnknownModel) {
  json body = {
      {"model", "nonexistent-model"},
      {"file", "/some/audio.mp3"},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/audio/transcriptions", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, AudioTranscriptionRejectsNonAudioModel) {
  // alpha-model has task="chat-completion", not "automatic-speech-recognition"
  auto audio_path = fl::test::GetTestDataPath("Recording.mp3");
  json body = {
      {"model", "alpha-model:1"},
      {"file", audio_path.string()},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/audio/transcriptions", body.dump()),
               std::exception);
}

TEST_F(WebServiceTest, AudioTranscriptionRejectsNonexistentFile) {
  // Use tiny-random-gpt2 which has task="chat-completion" — will fail on task check.
  // But even if an audio model were added, the file doesn't exist.
  json body = {
      {"model", "alpha-model:1"},
      {"file", "/nonexistent/path/audio.mp3"},
  };

  EXPECT_THROW(TestHttpPost(base_url_ + "/v1/audio/transcriptions", body.dump()),
               std::exception);
}

// ========================================================================
// Shutdown with keep-alive client
//
// Regression test for the ~120s stall in WebService::Stop() when a client is holding the connection open with
// HTTP keep-alive (default for httplib::Client, Node's OpenAI/LangChain SDKs, browsers, etc.).
//
// Without the ForceCloseConnectionProvider workaround, oatpp's HttpConnectionHandler::stop() would block in its
// polling loop until the per-connection worker thread's blocking recv() unblocks on its own — which on Windows
// only happens when the TCP keep-alive timer fires (~120s by default).
//
// This test uses its OWN WebService instance (not the shared fixture) so it can exercise Stop() in isolation.
// ========================================================================

TEST(WebServiceShutdownTest, StopReturnsQuicklyWithKeepAliveClient) {
  StderrLogger logger;
  test::CpuOnlyEpDetector ep_detector;
  ModelLoadManager model_load_manager(ep_detector, logger);
  SessionManager session_manager(logger);
  fl::test::NullTelemetry null_telemetry;
  test::MockCatalog catalog;

  WebService service(catalog, logger, "/tmp/test-cache", model_load_manager, session_manager, null_telemetry,
                     []() {});

  auto urls = service.Start({"http://127.0.0.1:0"});
  ASSERT_EQ(urls.size(), 1u);
  const std::string& base_url = urls[0];

  // httplib::Client uses HTTP keep-alive by default and reuses the underlying TCP connection between requests,
  // which is exactly the case that triggers the stall: after the response, oatpp's per-connection worker is
  // blocked in recv() waiting for the next pipelined request.
  httplib::Client client(base_url);
  client.set_connection_timeout(10, 0);
  client.set_read_timeout(10, 0);
  client.set_keep_alive(true);

  auto res = client.Get("/status");
  ASSERT_TRUE(res) << "GET /status failed: " << httplib::to_string(res.error());
  EXPECT_EQ(res->status, 200);

  // Intentionally do NOT close the client — leave the keep-alive connection open so the server-side worker is
  // sitting in a blocking recv() when Stop() runs.
  const auto stop_start = std::chrono::steady_clock::now();
  service.Stop();
  const auto stop_elapsed = std::chrono::steady_clock::now() - stop_start;
  const auto stop_seconds = std::chrono::duration_cast<std::chrono::seconds>(stop_elapsed).count();

  // Pre-fix behavior was ~120s on Windows (TCP keep-alive default). Allow generous headroom for slow CI while
  // still catching any regression to the old behavior.
  EXPECT_LT(stop_seconds, 30)
      << "WebService::Stop() took " << stop_seconds
      << "s with a keep-alive client connected. Expected <30s; pre-fix this was ~120s on Windows.";
}

#endif  // FOUNDRY_LOCAL_HAS_WEB_SERVICE
