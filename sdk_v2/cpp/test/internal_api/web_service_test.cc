// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for the oatpp web service handlers.

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE

#include "catalog.h"
#include "contracts/tool_definitions.h"
#include "ep_detection/ep_detector.h"
#include "http/http_client.h"
#include "inferencing/model_load_manager.h"
#include "inferencing/session/session_manager.h"
#include "internal_api/test_helpers.h"
#include "internal_api/test_model_cache.h"
#include "internal_api/toolcalling/coding_agent_tools_fixture.h"
#include "internal_api/web_service_test_helpers.h"
#include "logger.h"
#include "model.h"
#include "model_info.h"
#include "service/handler_utils.h"
#include "service/web_service.h"
#include "utils/temp_path.h"

#include <foundry_local/foundry_local_c.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <httplib.h>

using namespace fl;
using json = nlohmann::json;

namespace {

constexpr const char* kResponseStoreTestModelAlias = "response-store-test-model";

TEST(HttpUserAgentTest, KeepsOnlyKnownProductAndNumericVersion) {
  EXPECT_EQ(SafeHttpUserAgent("foundry-local-cpp/1.2.3"), "foundry-local-cpp/1.2.3");
  EXPECT_EQ(SafeHttpUserAgent("foundry-local-python/0.6"), "foundry-local-python/0.6");
  EXPECT_EQ(SafeHttpUserAgent("foundry-local-core/1"), "foundry-local-core/1");
  EXPECT_EQ(SafeHttpUserAgent("foundry-local-csharp/1.2"), "foundry-local-csharp/1.2");
  EXPECT_EQ(SafeHttpUserAgent("foundry-local-js/1.2"), "foundry-local-js/1.2");
  EXPECT_EQ(SafeHttpUserAgent("foundry-local-rust/1.2"), "foundry-local-rust/1.2");
  EXPECT_EQ(SafeHttpUserAgent(""), "unknown-http-client");
  EXPECT_EQ(SafeHttpUserAgent("telemetry-test-client"), "unknown-http-client");
  EXPECT_EQ(SafeHttpUserAgent("foundry-local-cpp/1.0 customer@example.com"), "unknown-http-client");
  EXPECT_EQ(SafeHttpUserAgent("foundry-local-cpp/1.0/secret"), "unknown-http-client");
  EXPECT_EQ(SafeHttpUserAgent("foundry-local-cpp/1.0-secret"), "unknown-http-client");
  EXPECT_EQ(SafeHttpUserAgent("foundry-local-cpp/1..2"), "unknown-http-client");
}

class WebUsageTelemetry : public TelemetryLogger {
 public:
  WebUsageTelemetry() : TelemetryLogger("test", fl::test::NullLog()) {}

  struct ActionCall {
    Action action;
    ActionStatus status;
    InvocationContext context;
    std::string model_id;
  };
  struct Snapshot {
    std::vector<ActionCall> actions;
    std::vector<ModelUsageInfo> models;
  };

  void RecordAction(Action action, ActionStatus status, const InvocationContext& context,
                    int64_t /*duration_ms*/, const std::string& model_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.actions.push_back({action, status, context, model_id});
  }

  void RecordModelUsage(const ModelUsageInfo& usage) override {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.models.push_back(usage);
  }

  Snapshot Events() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
  }

 private:
  mutable std::mutex mutex_;
  Snapshot events_;
};

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
    null_telemetry_ = std::make_unique<TelemetryLogger>("test", fl::test::NullLog());
    public_catalog_ = std::make_unique<test::MockCatalog>();
    local_catalog_ = std::make_unique<test::MockCatalog>();

    // Populate with test models
    public_catalog_->AddModel(Model::FromModelInfo(
        test::MakeTestModelInfo("alpha-model", "acme-corp"), "",
        svc_.download_manager, svc_.model_load_manager));
    public_catalog_->AddModel(Model::FromModelInfo(
        test::MakeTestModelInfo("beta-model", "contoso"), "",
        svc_.download_manager, svc_.model_load_manager));

    const auto loadable_model_path = test::GetTestDataModelPath(test::kLoadableTestModelAlias);
    ASSERT_TRUE(std::filesystem::exists(loadable_model_path))
        << "Expected loadable test model at " << loadable_model_path;
    public_catalog_->AddModel(Model::FromModelInfo(
        test::MakeTestModelInfo(test::kLoadableTestModelAlias, "microsoft"),
        loadable_model_path,
        svc_.download_manager,
        *model_load_manager_));

    const auto response_store_model_path = test::GetTestDataModelPath("tiny-paged-attention");
    ASSERT_TRUE(std::filesystem::exists(response_store_model_path))
        << "Expected response store test model at " << response_store_model_path;
    public_catalog_->AddModel(Model::FromModelInfo(
        test::MakeTestModelInfo(kResponseStoreTestModelAlias, "microsoft"),
        response_store_model_path,
        svc_.download_manager,
        *model_load_manager_));

    local_catalog_->AddModel(Model::FromModelInfo(
        test::MakeTestModelInfo("alpha-model", "local-owner"),
        loadable_model_path,
        svc_.download_manager,
        *model_load_manager_));
    local_catalog_->AddModel(Model::FromModelInfo(
        test::MakeTestModelInfo("local-model", "local-owner"), "",
        svc_.download_manager, svc_.model_load_manager));
    local_catalog_->AddModel(Model::FromModelInfo(
        test::MakeTestModelInfo(kResponseStoreTestModelAlias, "local-owner"),
        response_store_model_path,
        svc_.download_manager,
        *model_load_manager_));

    service_ = std::make_unique<WebService>(*public_catalog_, *local_catalog_, *logger_, "/tmp/test-cache",
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
    local_catalog_.reset();
    public_catalog_.reset();
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

  static std::unique_ptr<test::MockCatalog> public_catalog_;
  static std::unique_ptr<test::MockCatalog> local_catalog_;
  static std::unique_ptr<test::CpuOnlyEpDetector> ep_detector_;
  static std::unique_ptr<StderrLogger> logger_;
  static std::unique_ptr<ModelLoadManager> model_load_manager_;
  static std::unique_ptr<SessionManager> session_manager_;
  static std::unique_ptr<TelemetryLogger> null_telemetry_;
  static std::unique_ptr<WebService> service_;
  static std::string base_url_;
  static inline fl::test::FakeServiceBindings svc_;
};

// Static member definitions
std::unique_ptr<test::MockCatalog> WebServiceTest::public_catalog_;
std::unique_ptr<test::MockCatalog> WebServiceTest::local_catalog_;
std::unique_ptr<test::CpuOnlyEpDetector> WebServiceTest::ep_detector_;
std::unique_ptr<StderrLogger> WebServiceTest::logger_;
std::unique_ptr<ModelLoadManager> WebServiceTest::model_load_manager_;
std::unique_ptr<SessionManager> WebServiceTest::session_manager_;
std::unique_ptr<TelemetryLogger> WebServiceTest::null_telemetry_;
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
  auto* model = public_catalog_->GetModel(test::kLoadableTestModelAlias);
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
  auto* model = public_catalog_->GetModel(test::kLoadableTestModelAlias);
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
  EXPECT_EQ(j["data"].size(), 4u) << "Expected 4 models. Response: " << j.dump(2);
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

TEST_F(WebServiceTest, CatalogRoutesKeepDuplicateModelIdsSeparate) {
  const auto public_model = Get("/v1/models/alpha-model:1");
  const auto local_model = Get("/catalogs/local/v1/models/alpha-model:1");

  EXPECT_EQ(public_model["owned_by"], "acme-corp") << public_model.dump(2);
  EXPECT_EQ(local_model["owned_by"], "local-owner") << local_model.dump(2);
  EXPECT_THROW(TestHttpGet(base_url_ + "/v1/models/local-model:1"), std::exception);
  EXPECT_THROW(TestHttpGet(base_url_ + "/catalogs/local/v1/models/beta-model:1"), std::exception);
}

TEST_F(WebServiceTest, LocalModelManagementUsesOnlyLocalCatalog) {
  auto* model = local_catalog_->GetModel("alpha-model");
  ASSERT_NE(model, nullptr);
  ASSERT_TRUE(model->IsCached());

  const auto loaded = Get("/catalogs/local/models/load/alpha-model");
  EXPECT_EQ(loaded["status"], "loaded") << loaded.dump(2);
  EXPECT_TRUE(model->IsLoaded());

  const auto local_models = Get("/catalogs/local/models/loaded");
  ASSERT_EQ(local_models.size(), 1u) << local_models.dump(2);
  EXPECT_EQ(local_models[0], "alpha-model:1") << local_models.dump(2);

  const auto public_models = Get("/models/loaded");
  EXPECT_TRUE(public_models.empty()) << public_models.dump(2);

  const auto unloaded = Get("/catalogs/local/models/unload/alpha-model");
  EXPECT_EQ(unloaded["status"], "unloaded") << unloaded.dump(2);
  EXPECT_FALSE(model->IsLoaded());
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
  TelemetryLogger null_telemetry{"test", fl::test::NullLog()};

  WebService service(catalog, catalog, logger, "/tmp/test", model_load_manager, session_manager, null_telemetry,
                     []() {});
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
  TelemetryLogger null_telemetry{"test", fl::test::NullLog()};

  WebService service(catalog, catalog, logger, "/tmp/test", model_load_manager, session_manager, null_telemetry,
                     []() {});
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
  TelemetryLogger null_telemetry{"test", fl::test::NullLog()};

  WebService service(catalog, catalog, logger, "/tmp/test", model_load_manager, session_manager, null_telemetry,
                     []() {});
  // Should not crash
  service.Stop();
}

TEST(WebServiceLifecycleTest, MultipleEndpoints) {
  test::MockCatalog catalog;
  StderrLogger logger;
  test::CpuOnlyEpDetector ep_detector;
  ModelLoadManager model_load_manager(ep_detector, logger);
  SessionManager session_manager(logger);
  TelemetryLogger null_telemetry{"test", fl::test::NullLog()};

  WebService service(catalog, catalog, logger, "/tmp/test", model_load_manager, session_manager, null_telemetry,
                     []() {});
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
  TelemetryLogger null_telemetry{"test", fl::test::NullLog()};

  WebService service(catalog, catalog, logger, "/tmp/test", model_load_manager, session_manager, null_telemetry,
                     []() {});
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
  TelemetryLogger null_telemetry{"test", fl::test::NullLog()};

  WebService service(catalog, catalog, logger, "/tmp/test", model_load_manager, session_manager, null_telemetry,
                     []() {});
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
// Tool declarations over HTTP
//
// These exercise the boundary rather than generation: a request is accepted only once its tools
// have been parsed and registered, so "rejected with 400" and "got as far as model loading" are
// exactly the two outcomes that distinguish a declaration this runtime can honour from one it
// cannot.
// ========================================================================

namespace {

struct HttpResult {
  int status = 0;
  json body;
};

/// POST a JSON body and report the status alongside the parsed body.
///
/// Uses the product's own HTTP client rather than httplib: httplib keeps the connection alive and
/// stalls against this server once a request body grows past a few kilobytes, which a realistic
/// tool inventory does immediately.
HttpResult PostJson(const std::string& url, const json& body) {
  http::HttpRequestOptions options;
  options.close_connection = true;

  auto response = http::HttpPostWithResponse(url, body.dump(), options);
  EXPECT_NE(response.status, 0) << "transport failure: " << response.body;

  HttpResult result;
  result.status = response.status;
  result.body = json::parse(response.body, nullptr, /*allow_exceptions=*/false);
  return result;
}

HttpResult GetJson(const std::string& url) {
  http::HttpRequestOptions options;
  options.close_connection = true;

  auto response = http::HttpGetWithResponse(url, options);
  EXPECT_NE(response.status, 0) << "transport failure: " << response.body;

  HttpResult result;
  result.status = response.status;
  result.body = json::parse(response.body, nullptr, /*allow_exceptions=*/false);
  return result;
}

/// The `message` of an OpenAI-style error body, or the failed response's error message.
std::string ErrorMessageOf(const json& body) {
  if (body.contains("error") && body["error"].is_object()) {
    return body["error"].value("message", "");
  }

  return "";
}

}  // namespace

TEST_F(WebServiceTest, LocalInferenceRoutesResolveOnlyLocalModels) {
  const auto audio_path = fl::test::GetTestDataPath("Recording.mp3");
  const std::vector<std::pair<std::string, json>> requests = {
      {"/catalogs/local/v1/chat/completions",
       {
           {"model", "local-model:1"},
           {"messages", json::array({{{"role", "user"}, {"content", "hello"}}})},
       }},
      {"/catalogs/local/v1/responses",
       {
           {"model", "local-model:1"},
           {"input", "hello"},
       }},
      {"/catalogs/local/v1/embeddings",
       {
           {"model", "local-model:1"},
           {"input", "hello"},
       }},
      {"/catalogs/local/v1/audio/transcriptions",
       {
           {"model", "local-model:1"},
           {"filename", audio_path.string()},
       }},
  };

  for (const auto& [endpoint, body] : requests) {
    SCOPED_TRACE(endpoint);
    const auto result = PostJson(base_url_ + endpoint, body);

    EXPECT_EQ(result.status, 400) << result.body.dump(2);
    EXPECT_NE(ErrorMessageOf(result.body).find("not loaded"), std::string::npos)
        << result.body.dump(2);
  }

  const auto public_result = PostJson(
      base_url_ + "/v1/chat/completions",
      {
          {"model", "local-model:1"},
          {"messages", json::array({{{"role", "user"}, {"content", "hello"}}})},
      });
  EXPECT_EQ(public_result.status, 404) << public_result.body.dump(2);
}

TEST_F(WebServiceTest, LocalStoredResponsesRemainIsolatedFromPublicRoutes) {
  auto* model = local_catalog_->GetModel(kResponseStoreTestModelAlias);
  ASSERT_NE(model, nullptr);

  const auto load_result = Get(std::string("/catalogs/local/models/load/") + kResponseStoreTestModelAlias);
  ASSERT_EQ(load_result["status"], "loaded") << load_result.dump(2);

  const std::string model_id = std::string(kResponseStoreTestModelAlias) + ":1";
  const auto first = PostJson(
      base_url_ + "/catalogs/local/v1/responses",
      {
          {"model", model_id},
          {"input", "first turn"},
          {"store", true},
          {"max_output_tokens", 4},
          {"temperature", 0},
      });
  ASSERT_EQ(first.status, 200) << first.body.dump(2);
  const auto first_id = first.body.at("id").get<std::string>();

  const auto local_list = Get("/catalogs/local/v1/responses");
  ASSERT_EQ(local_list["data"].size(), 1u) << local_list.dump(2);
  EXPECT_EQ(local_list["data"][0]["id"], first_id) << local_list.dump(2);
  EXPECT_TRUE(Get("/v1/responses")["data"].empty());

  const auto local_retrieved = GetJson(base_url_ + "/catalogs/local/v1/responses/" + first_id);
  EXPECT_EQ(local_retrieved.status, 200) << local_retrieved.body.dump(2);
  EXPECT_EQ(local_retrieved.body["id"], first_id) << local_retrieved.body.dump(2);

  const auto public_retrieved = GetJson(base_url_ + "/v1/responses/" + first_id);
  EXPECT_EQ(public_retrieved.status, 404) << public_retrieved.body.dump(2);

  const json continuation_body = {
      {"model", model_id},
      {"previous_response_id", first_id},
      {"input", "second turn"},
      {"store", true},
      {"max_output_tokens", 4},
      {"temperature", 0},
  };
  const auto local_continuation = PostJson(base_url_ + "/catalogs/local/v1/responses", continuation_body);
  ASSERT_EQ(local_continuation.status, 200) << local_continuation.body.dump(2);
  const auto continuation_id = local_continuation.body.at("id").get<std::string>();
  EXPECT_EQ(local_continuation.body["previous_response_id"], first_id)
      << local_continuation.body.dump(2);

  const auto public_continuation = PostJson(base_url_ + "/v1/responses", continuation_body);
  EXPECT_EQ(public_continuation.status, 404) << public_continuation.body.dump(2);
  EXPECT_NE(ErrorMessageOf(public_continuation.body).find("Previous response not found"), std::string::npos)
      << public_continuation.body.dump(2);

  const auto deleted =
      json::parse(TestHttpDelete(base_url_ + "/catalogs/local/v1/responses/" + first_id));
  EXPECT_TRUE(deleted["deleted"].get<bool>()) << deleted.dump(2);
  EXPECT_EQ(GetJson(base_url_ + "/catalogs/local/v1/responses/" + first_id).status, 404);
  EXPECT_EQ(GetJson(base_url_ + "/catalogs/local/v1/responses/" + continuation_id).status, 404);
  EXPECT_TRUE(Get("/catalogs/local/v1/responses")["data"].empty());

  const auto unload_result = Get(std::string("/catalogs/local/models/unload/") + kResponseStoreTestModelAlias);
  EXPECT_EQ(unload_result["status"], "unloaded") << unload_result.dump(2);
}

TEST_F(WebServiceTest, ClosingResponsesStreamCancelsInferenceAndPreservesConversation) {
  const auto load_result = Get(std::string("/models/load/") + kResponseStoreTestModelAlias);
  ASSERT_EQ(load_result["status"], "loaded") << load_result.dump(2);

  const auto root = PostJson(base_url_ + "/v1/responses",
                             {{"model", std::string(kResponseStoreTestModelAlias) + ":1"},
                              {"input", "Start a conversation"},
                              {"store", true},
                              {"max_output_tokens", 4}});
  ASSERT_EQ(root.status, 200) << root.body.dump(2);
  const auto root_id = root.body.at("id").get<std::string>();

  httplib::Client client(base_url_);
  client.set_read_timeout(10, 0);
  bool received_event = false;
  bool was_active = false;
  const json streaming_request = {
      {"model", std::string(kResponseStoreTestModelAlias) + ":1"},
      {"previous_response_id", root_id},
      {"input", "Continue the conversation"},
      {"max_output_tokens", 512},
      {"stream", true},
  };

  std::string created_response_id;
  std::string events;
  const auto result = client.Post(
      "/v1/responses", httplib::Headers{}, streaming_request.dump(), "application/json",
      [&](const char* data, size_t length) {
        events.append(data, length);
        const auto event_pos = events.find("event: response.created\ndata: ");
        if (event_pos == std::string::npos) {
          return true;
        }

        const auto json_start = event_pos + std::string("event: response.created\ndata: ").size();
        const auto json_end = events.find('\n', json_start);
        if (json_end == std::string::npos) {
          return true;
        }

        created_response_id = json::parse(events.substr(json_start, json_end - json_start)).at("response").at("id");
        received_event = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (session_manager_->ActiveCount() == 0 && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        was_active = session_manager_->ActiveCount() > 0;
        return false;  // close the socket before the generation completes
      });

  EXPECT_FALSE(result);
  ASSERT_TRUE(received_event);
  ASSERT_TRUE(was_active) << "The SSE connection closed before inference started";

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (session_manager_->ActiveCount() > 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(session_manager_->ActiveCount(), 0u) << "Disconnected inference did not stop";
  ASSERT_FALSE(created_response_id.empty());
  EXPECT_EQ(GetJson(base_url_ + "/v1/responses/" + created_response_id).status, 404)
      << "Canceled inference must not publish a completed response";

  const auto recovered = PostJson(base_url_ + "/v1/responses",
                                  {{"model", std::string(kResponseStoreTestModelAlias) + ":1"},
                                   {"previous_response_id", root_id},
                                   {"input", "A fresh continuation"},
                                   {"max_output_tokens", 4}});
  EXPECT_EQ(recovered.status, 200) << recovered.body.dump(2);
  EXPECT_EQ(recovered.body.value("status", ""), "completed");

  const json completed_stream = {
      {"model", std::string(kResponseStoreTestModelAlias) + ":1"},
      {"input", "Another conversation"},
      {"max_output_tokens", 4},
      {"stream", true},
  };
  const auto completed = client.Post("/v1/responses", completed_stream.dump(), "application/json");
  ASSERT_TRUE(completed) << httplib::to_string(completed.error());
  EXPECT_EQ(completed->status, 200);
  EXPECT_NE(completed->body.find("event: response.completed"), std::string::npos);
  EXPECT_NE(completed->body.find("data: [DONE]"), std::string::npos);
}

TEST_F(WebServiceTest, StreamingChatCompletionsRejectsModifiedStockLarkGrammarBeforeModelResolution) {
  json body = {
      {"model", "alpha-model"},  // in the catalog, never loadable in this fixture
      {"messages", json::array({{{"role", "user"}, {"content", "hi"}}})},
      {"tools", json::parse(test::kCodingAgentChatToolsJson)},
      {"stream", true},
  };
  body["tools"][0]["custom"]["format"]["grammar"]["definition"] =
      std::string(tools::kStockGhcpApplyPatchLarkGrammar) + "\n";

  auto result = PostJson(base_url_ + "/v1/chat/completions", body);

  EXPECT_EQ(result.status, 400) << result.body.dump(2);
  EXPECT_NE(ErrorMessageOf(result.body).find("stock GHCP apply-patch"), std::string::npos)
      << result.body.dump(2);
}

TEST_F(WebServiceTest, ChatCompletionsStrictContractIsValidatedBeforeModelResolution) {
  for (const bool stream : {false, true}) {
    for (const auto& [strict, expected_status] :
         std::vector<std::pair<json, int>>{{nullptr, 404}, {false, 404}, {true, 400}}) {
      const json body = {
          {"model", "alpha-model"},
          {"messages", json::array({{{"role", "user"}, {"content", "hi"}}})},
          {"tools",
           json::array({{{"type", "function"},
                         {"function",
                          {{"name", "lookup"},
                           {"parameters", json::object()},
                           {"strict", strict}}}}})},
          {"stream", stream},
      };

      const auto result = PostJson(base_url_ + "/v1/chat/completions", body);
      EXPECT_EQ(result.status, expected_status) << body.dump() << '\n'
                                                << result.body.dump(2);
      if (strict == true) {
        EXPECT_NE(ErrorMessageOf(result.body).find("constrained decoding"), std::string::npos)
            << result.body.dump(2);
      }
    }
  }
}

TEST_F(WebServiceTest, ResponsesStrictContractIsValidatedBeforeModelResolution) {
  for (const bool stream : {false, true}) {
    for (const auto& [strict, expected_status] :
         std::vector<std::pair<json, int>>{{nullptr, 404}, {false, 404}, {true, 400}}) {
      const json body = {
          {"model", "alpha-model"},
          {"input", "hi"},
          {"tools",
           json::array({{{"type", "function"},
                         {"name", "lookup"},
                         {"parameters", json::object()},
                         {"strict", strict}}})},
          {"stream", stream},
      };

      const auto result = PostJson(base_url_ + "/v1/responses", body);
      EXPECT_EQ(result.status, expected_status) << body.dump() << '\n'
                                                << result.body.dump(2);
      if (strict == true) {
        EXPECT_NE(ErrorMessageOf(result.body).find("constrained decoding"), std::string::npos)
            << result.body.dump(2);
      }
    }
  }
}

TEST_F(WebServiceTest, ChatCompletionsAcceptsTextCustomToolDeclaration) {
  json body = {
      {"model", "alpha-model"},  // in the catalog, never loadable in this fixture
      {"messages", json::array({{{"role", "user"}, {"content", "hi"}}})},
      {"tools", json::array({{{"type", "custom"},
                              {"custom", {{"name", "apply_patch"}, {"format", {{"type", "text"}}}}}}})},
  };

  auto result = PostJson(base_url_ + "/v1/chat/completions", body);

  // The declaration was accepted: the request got as far as model resolution, and the only
  // complaint is the model, not the tools.
  EXPECT_EQ(result.status, 404) << result.body.dump(2);
  EXPECT_NE(ErrorMessageOf(result.body).find("No model matching"), std::string::npos) << result.body.dump(2);
}

TEST_F(WebServiceTest, StreamingChatCompletionsRejectsRequiredChoiceWithoutToolsBeforeSse) {
  const json body = {
      {"model", "alpha-model"},
      {"messages", json::array({{{"role", "user"}, {"content", "hi"}}})},
      {"tool_choice", "required"},
      {"stream", true},
  };

  const auto result = PostJson(base_url_ + "/v1/chat/completions", body);

  EXPECT_EQ(result.status, 400) << result.body.dump(2);
  EXPECT_NE(ErrorMessageOf(result.body).find("at least one declared tool"), std::string::npos)
      << result.body.dump(2);
}

TEST_F(WebServiceTest, ResponsesRejectsModifiedStockCustomToolGrammarFormat) {
  json body = {
      {"model", "alpha-model"},  // in the catalog, never loadable in this fixture
      {"input", "hi"},
      {"tools", json::parse(test::kCodingAgentResponsesToolsJson)},
  };
  body["tools"][0]["format"]["definition"] = std::string(tools::kStockGhcpApplyPatchLarkGrammar) + "\n";

  auto result = PostJson(base_url_ + "/v1/responses", body);

  EXPECT_EQ(result.status, 400) << result.body.dump(2);
  EXPECT_NE(ErrorMessageOf(result.body).find("stock GHCP apply-patch"), std::string::npos)
      << result.body.dump(2);
}

TEST_F(WebServiceTest, MalformedRawEnvelopeDescriptorIsRejectedBeforeModelResolutionOrSse) {
  for (const bool chat : {false, true}) {
    for (const bool stream : {false, true}) {
      json body = chat
                      ? json{{"model", "alpha-model"},
                             {"messages", json::array({{{"role", "user"}, {"content", "hi"}}})}}
                      : json{{"model", "alpha-model"}, {"input", "hi"}};
      body["stream"] = stream;
      body["metadata"] = {
          {tools::kRawEnvelopeMetadataKey,
           R"({"type":"raw_envelope","tool_name":"edit","start_marker":"BEGIN"})"}};

      const auto endpoint = chat ? "/v1/chat/completions" : "/v1/responses";
      const auto result = PostJson(base_url_ + endpoint, body);

      EXPECT_EQ(result.status, 400) << body.dump() << '\n'
                                    << result.body.dump(2);
      EXPECT_NE(ErrorMessageOf(result.body).find(tools::kRawEnvelopeMetadataKey),
                std::string::npos)
          << result.body.dump(2);
    }
  }
}

TEST_F(WebServiceTest, ResponsesRejectsNonStringRawEnvelopeDescriptorBeforeModelResolutionOrSse) {
  for (const bool stream : {false, true}) {
    const json body = {
        {"model", "alpha-model"},
        {"input", "hi"},
        {"stream", stream},
        {"metadata", {{tools::kRawEnvelopeMetadataKey, {{"type", "raw_envelope"}}}}},
    };

    const auto result = PostJson(base_url_ + "/v1/responses", body);

    EXPECT_EQ(result.status, 400) << body.dump() << '\n'
                                  << result.body.dump(2);
    EXPECT_NE(ErrorMessageOf(result.body).find(tools::kRawEnvelopeMetadataKey),
              std::string::npos)
        << result.body.dump(2);
  }
}

TEST_F(WebServiceTest, RawEnvelopeDescriptorMustReferenceEffectiveCustomTool) {
  const std::string descriptor =
      R"({"type":"raw_envelope","tool_name":"edit","start_marker":"BEGIN","end_marker":"END"})";
  for (const bool chat : {false, true}) {
    for (const bool stream : {false, true}) {
      json body = chat
                      ? json{{"model", "alpha-model"},
                             {"messages", json::array({{{"role", "user"}, {"content", "hi"}}})},
                             {"tools",
                              json::array({
                                  {{"type", "custom"},
                                   {"custom", {{"name", "edit"}, {"format", {{"type", "text"}}}}}},
                                  {{"type", "custom"},
                                   {"custom", {{"name", "other"}, {"format", {{"type", "text"}}}}}},
                              })},
                             {"tool_choice",
                              {{"type", "custom"}, {"custom", {{"name", "other"}}}}}}
                      : json{{"model", "alpha-model"},
                             {"input", "hi"},
                             {"tools",
                              json::array({
                                  {{"type", "custom"}, {"name", "edit"}},
                                  {{"type", "custom"}, {"name", "other"}},
                              })},
                             {"tool_choice", {{"type", "custom"}, {"name", "other"}}}};
      body["stream"] = stream;
      body["metadata"] = {{tools::kRawEnvelopeMetadataKey, descriptor}};

      const auto endpoint = chat ? "/v1/chat/completions" : "/v1/responses";
      const auto result = PostJson(base_url_ + endpoint, body);

      EXPECT_EQ(result.status, 400) << body.dump() << '\n'
                                    << result.body.dump(2);
      EXPECT_NE(ErrorMessageOf(result.body).find("effective declared custom tool"),
                std::string::npos)
          << result.body.dump(2);
    }
  }
}

TEST_F(WebServiceTest, ConflictingStockRawEnvelopeDescriptorIsRejectedBeforeModelResolutionOrSse) {
  const std::vector<std::string> descriptors{
      R"({"type":"raw_envelope","tool_name":"apply_patch","start_marker":"BEGIN","end_marker":"END"})",
      R"({"type":"raw_envelope","tool_name":"edit","start_marker":"BEGIN","end_marker":"END"})",
  };
  for (const auto& descriptor : descriptors) {
    for (const bool chat : {false, true}) {
      for (const bool stream : {false, true}) {
        json body = chat
                        ? json{{"model", "alpha-model"},
                               {"messages", json::array({{{"role", "user"}, {"content", "hi"}}})},
                               {"tools", json::parse(test::kCodingAgentChatToolsJson)}}
                        : json{{"model", "alpha-model"},
                               {"input", "hi"},
                               {"tools", json::parse(test::kCodingAgentResponsesToolsJson)}};
        body["tools"].push_back(
            chat ? json{{"type", "custom"},
                        {"custom", {{"name", "edit"}, {"format", {{"type", "text"}}}}}}
                 : json{{"type", "custom"}, {"name", "edit"}});
        body["stream"] = stream;
        body["metadata"] = {{tools::kRawEnvelopeMetadataKey, descriptor}};

        const auto endpoint = chat ? "/v1/chat/completions" : "/v1/responses";
        const auto result = PostJson(base_url_ + endpoint, body);

        EXPECT_EQ(result.status, 400) << body.dump() << '\n'
                                      << result.body.dump(2);
        EXPECT_NE(ErrorMessageOf(result.body).find("conflicts with the stock apply_patch grammar"),
                  std::string::npos)
            << result.body.dump(2);
      }
    }
  }
}

TEST_F(WebServiceTest, GenericRawEnvelopeDescriptorIsAcceptedForAutoAndForcedCustomTool) {
  const std::string descriptor =
      R"({"type":"raw_envelope","tool_name":"edit","start_marker":"BEGIN","end_marker":"END"})";
  for (const bool forced : {false, true}) {
    json body = {
        {"model", "alpha-model"},
        {"messages", json::array({{{"role", "user"}, {"content", "hi"}}})},
        {"tools",
         json::array({{{"type", "custom"},
                       {"custom", {{"name", "edit"}, {"format", {{"type", "text"}}}}}}})},
        {"metadata", {{tools::kRawEnvelopeMetadataKey, descriptor}}},
    };
    if (forced) {
      body["tool_choice"] = {
          {"type", "custom"}, {"custom", {{"name", "edit"}}}};
    }

    const auto result = PostJson(base_url_ + "/v1/chat/completions", body);

    EXPECT_EQ(result.status, 404) << result.body.dump(2);
    EXPECT_NE(ErrorMessageOf(result.body).find("No model matching"), std::string::npos)
        << result.body.dump(2);
  }
}

TEST_F(WebServiceTest, ResponsesRejectsForcedToolRemovedByAllowedFilter) {
  const json body = {
      {"model", "alpha-model"},
      {"input", "hi"},
      {"tools", json::array({{{"type", "function"}, {"name", "a"}, {"parameters", json::object()}},
                             {{"type", "function"}, {"name", "b"}, {"parameters", json::object()}}})},
      {"tool_choice", {{"type", "function"}, {"name", "a"}}},
      {"allowed_tools", json::array({"b"})},
  };

  const auto result = PostJson(base_url_ + "/v1/responses", body);

  EXPECT_EQ(result.status, 400) << result.body.dump(2);
  EXPECT_NE(ErrorMessageOf(result.body).find("at least one effective tool"), std::string::npos)
      << result.body.dump(2);
}

TEST_F(WebServiceTest, ResponsesRejectsRequiredAllowedToolsWithNoEffectiveTools) {
  const json body = {
      {"model", "alpha-model"},
      {"input", "hi"},
      {"tools", json::array({{{"type", "function"}, {"name", "a"}, {"parameters", json::object()}}})},
      {"tool_choice",
       {{"type", "allowed_tools"},
        {"mode", "required"},
        {"tools", json::array({{{"type", "custom"}, {"name", "a"}}})}}},
  };

  const auto result = PostJson(base_url_ + "/v1/responses", body);

  EXPECT_EQ(result.status, 400) << result.body.dump(2);
  EXPECT_NE(ErrorMessageOf(result.body).find("wrong tool kind"), std::string::npos)
      << result.body.dump(2);
}

TEST_F(WebServiceTest, ResponsesRejectsMalformedToolItemsBeforeModelResolution) {
  for (const auto& item : {
           json{{"type", "function_call"}, {"call_id", ""}, {"name", "f"}, {"arguments", "{}"}},
           json{{"type", "function_call"}, {"call_id", "c"}, {"name", ""}, {"arguments", "{}"}},
           json{{"type", "custom_tool_call"}, {"call_id", ""}, {"name", "c"}, {"input", "raw"}},
           json{{"type", "custom_tool_call"}, {"call_id", "c"}, {"name", ""}, {"input", "raw"}},
           json{{"type", "custom_tool_call"}, {"call_id", "c"}, {"name", "c"}},
           json{{"type", "custom_tool_call"}, {"call_id", "c"}, {"name", "c"}, {"input", nullptr}},
           json{{"type", "function_call_output"}, {"call_id", ""}, {"output", "done"}},
           json{{"type", "custom_tool_call_output"}, {"call_id", ""}, {"output", "done"}}}) {
    const json body = {
        {"model", "alpha-model"},
        {"input", json::array({item})},
    };

    const auto result = PostJson(base_url_ + "/v1/responses", body);
    const auto expects_input_error =
        item.at("type") == "custom_tool_call" &&
        (!item.contains("input") || item.at("input").is_null());
    const auto expected_error =
        expects_input_error ? "required and must be a string" : "non-empty string";

    EXPECT_EQ(result.status, 400) << item.dump() << '\n'
                                  << result.body.dump(2);
    EXPECT_NE(ErrorMessageOf(result.body).find(expected_error), std::string::npos)
        << item.dump() << '\n'
        << result.body.dump(2);
  }
}

TEST_F(WebServiceTest, ChatCompletionsCustomCallRequiresStringInputButAcceptsEmptyString) {
  const auto make_body = [](const json& custom) {
    return json{
        {"model", "alpha-model"},
        {"messages",
         json::array({
             {{"role", "assistant"},
              {"tool_calls",
               json::array({{{"id", "call_1"}, {"type", "custom"}, {"custom", custom}}})}},
             {{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", "done"}},
         })},
    };
  };

  for (const auto& custom : {json{{"name", "apply_patch"}},
                             json{{"name", "apply_patch"}, {"input", nullptr}}}) {
    const auto result = PostJson(base_url_ + "/v1/chat/completions", make_body(custom));
    EXPECT_EQ(result.status, 400) << result.body.dump(2);
  }

  const auto accepted = PostJson(
      base_url_ + "/v1/chat/completions",
      make_body(json{{"name", "apply_patch"}, {"input", ""}}));
  EXPECT_EQ(accepted.status, 404) << accepted.body.dump(2);
}

TEST_F(WebServiceTest, ResponsesRejectsUnknownOfficialAllowedToolBeforeModelResolution) {
  const json body = {
      {"model", "alpha-model"},
      {"input", "hi"},
      {"tools", json::array({{{"type", "function"}, {"name", "a"}, {"parameters", json::object()}}})},
      {"tool_choice",
       {{"type", "allowed_tools"},
        {"mode", "auto"},
        {"tools", json::array({{{"type", "function"}, {"name", "missing"}}})}}},
  };

  const auto result = PostJson(base_url_ + "/v1/responses", body);

  EXPECT_EQ(result.status, 400) << result.body.dump(2);
  EXPECT_NE(ErrorMessageOf(result.body).find("undeclared tool"), std::string::npos)
      << result.body.dump(2);
}

TEST_F(WebServiceTest, ChatCompletionsRejectsMalformedCustomToolGrammarFormat) {
  json body = {
      {"model", test::kLoadableTestModelAlias},
      {"messages", json::array({{{"role", "user"}, {"content", "hi"}}})},
      {"tools",
       json::array({{{"type", "custom"},
                     {"custom",
                      {{"name", "apply_patch"},
                       {"format", {{"type", "grammar"}, {"grammar", {{"syntax", "lark"}}}}}}}}})},
  };

  auto result = PostJson(base_url_ + "/v1/chat/completions", body);

  EXPECT_EQ(result.status, 400) << result.body.dump(2);
  EXPECT_NE(ErrorMessageOf(result.body).find("apply_patch"), std::string::npos) << result.body.dump(2);
}

TEST_F(WebServiceTest, ResponsesRejectsMalformedCustomToolGrammarFormat) {
  json body = {
      {"model", test::kLoadableTestModelAlias},
      {"input", "hi"},
      {"tools", json::array({{{"type", "custom"},
                              {"name", "apply_patch"},
                              {"format", {{"type", "grammar"}, {"syntax", "lark"}}}}})},
  };

  auto result = PostJson(base_url_ + "/v1/responses", body);

  EXPECT_EQ(result.status, 400) << result.body.dump(2);
  EXPECT_NE(ErrorMessageOf(result.body).find("apply_patch"), std::string::npos) << result.body.dump(2);
}

TEST_F(WebServiceTest, ResponsesRejectsUnknownToolTypeAsBadRequest) {
  // An unsupported tool type is a client mistake and must surface as a 400 naming the type, not be silently
  // accepted as a function tool. Mirrors the Chat Completions surface.
  json body = {
      {"model", test::kLoadableTestModelAlias},
      {"input", "hi"},
      {"tools", json::array({{{"type", "web_search"}}})},
  };

  auto result = PostJson(base_url_ + "/v1/responses", body);

  EXPECT_EQ(result.status, 400) << result.body.dump(2);
  EXPECT_NE(ErrorMessageOf(result.body).find("web_search"), std::string::npos) << result.body.dump(2);
}

TEST_F(WebServiceTest, ChatCompletionsRejectsUnknownToolTypeAsBadRequest) {
  json body = {
      {"model", test::kLoadableTestModelAlias},
      {"messages", json::array({{{"role", "user"}, {"content", "hi"}}})},
      {"tools", json::array({{{"type", "web_search"}}})},
  };

  auto result = PostJson(base_url_ + "/v1/chat/completions", body);

  EXPECT_EQ(result.status, 400) << result.body.dump(2);
  EXPECT_NE(ErrorMessageOf(result.body).find("web_search"), std::string::npos) << result.body.dump(2);
}

TEST_F(WebServiceTest, ResponsesAcceptsCustomToolCallTranscriptItems) {
  for (const auto& input : {json("PATCH BODY"), json("")}) {
    json body = {
        {"model", "alpha-model"},  // in the catalog, never loadable in this fixture
        {"input", json::array({
                      {{"type", "message"}, {"role", "user"}, {"content", "patch it"}},
                      {{"type", "custom_tool_call"},
                       {"call_id", "call_2"},
                       {"name", "apply_patch"},
                       {"input", input}},
                      {{"type", "custom_tool_call_output"}, {"call_id", "call_2"}, {"output", "applied"}},
                  })},
        {"tools", json::array({{{"type", "custom"}, {"name", "apply_patch"}}})},
    };

    const auto result = PostJson(base_url_ + "/v1/responses", body);

    // Parsed fine — the only complaint is the model, not the transcript items.
    EXPECT_EQ(result.status, 404) << result.body.dump(2);
    EXPECT_NE(ErrorMessageOf(result.body).find("No model matching"), std::string::npos)
        << result.body.dump(2);
  }
}

TEST_F(WebServiceTest, ResponsesRejectedToolDeclarationIsAClientErrorAndStoresNothing) {
  // Declaring the same tool name twice is rejected by the registry, which happens after the request has been
  // accepted and a response id minted. It is the caller's mistake, so it comes back as a 400 error envelope rather
  // than a stored `failed` response — and nothing about the turn may reach the store.
  auto load_result = Get(std::string("/models/load/") + test::kLoadableTestModelAlias);
  ASSERT_EQ(load_result["status"], "loaded") << "Response: " << load_result.dump(2);

  const auto before = Get("/v1/responses")["data"].size();

  json body = {
      {"model", std::string(test::kLoadableTestModelAlias) + ":1"},
      {"input", "hi"},
      {"store", true},
      {"tools", json::array({{{"type", "custom"}, {"name", "apply_patch"}},
                             {{"type", "function"}, {"name", "apply_patch"}, {"parameters", json::object()}}})},
  };

  auto result = PostJson(base_url_ + "/v1/responses", body);

  ASSERT_EQ(result.status, 400) << result.body.dump(2);
  ASSERT_TRUE(result.body.contains("error")) << result.body.dump(2);
  EXPECT_EQ(result.body["error"].value("type", ""), "invalid_request_error") << result.body.dump(2);
  EXPECT_NE(ErrorMessageOf(result.body).find("apply_patch"), std::string::npos) << result.body.dump(2);

  // A rejected turn leaves the store exactly as it was.
  EXPECT_EQ(Get("/v1/responses")["data"].size(), before);
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
  TelemetryLogger null_telemetry{"test", fl::test::NullLog()};
  test::MockCatalog catalog;

  WebService service(catalog, catalog, logger, "/tmp/test-cache", model_load_manager, session_manager, null_telemetry,
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

namespace {

struct RouteTelemetryCase {
  const char* method;
  const char* path;
  const char* body;
  Action action;
  int status;
};

class WebServiceTelemetryTest : public ::testing::TestWithParam<RouteTelemetryCase> {};

}  // namespace

TEST_P(WebServiceTelemetryTest, ClientErrorRetainsHttpResponseAndRecordsDirectAttribution) {
  test::MockCatalog catalog;
  test::CpuOnlyEpDetector ep_detector;
  ModelLoadManager load_manager(ep_detector, fl::test::NullLog());
  SessionManager sessions(fl::test::NullLog());
  WebUsageTelemetry telemetry;
  auto cache = test::TempPath::CreateTempDir("fl_route_telemetry_");
  WebService service(catalog, catalog, fl::test::NullLog(), cache.string(), load_manager, sessions, telemetry,
                     []() {});
  const auto urls = service.Start({"http://127.0.0.1:0"});
  ASSERT_EQ(urls.size(), 1u);
  httplib::Client client(urls[0]);
  client.set_read_timeout(10, 0);
  const auto& scenario = GetParam();
  const httplib::Headers headers{{"User-Agent", "telemetry-test-client"}};
  const auto response = std::string(scenario.method) == "POST"
                            ? client.Post(scenario.path, headers, scenario.body, "application/json")
                        : std::string(scenario.method) == "DELETE" ? client.Delete(scenario.path, headers)
                                                                   : client.Get(scenario.path, headers);
  ASSERT_TRUE(response) << httplib::to_string(response.error());
  EXPECT_EQ(response->status, scenario.status);
  const auto body = json::parse(response->body);
  EXPECT_EQ(body.at("error").at("type"), "invalid_request_error");
  service.Stop();

  const auto events = telemetry.Events();
  ASSERT_EQ(events.actions.size(), 1u);
  const auto& action = events.actions[0];
  EXPECT_EQ(action.action, scenario.action);
  EXPECT_EQ(action.status, ActionStatus::kClientError);
  EXPECT_EQ(action.context.user_agent, "unknown-http-client");
  EXPECT_EQ(action.context.correlation_id.size(), 36u);
  EXPECT_FALSE(action.context.indirect);
  EXPECT_TRUE(events.models.empty());
}

TEST(WebServiceTelemetryTest, KnownModelManagementOutcomesRecordResolvedModelId) {
  test::FakeServiceBindings bindings;
  test::MockCatalog catalog;
  catalog.AddModel(Model::FromModelInfo(test::MakeTestModelInfo("alpha-model"), "", bindings.download_manager,
                                        bindings.model_load_manager));
  SessionManager sessions(bindings.logger);
  WebUsageTelemetry telemetry;
  auto cache = test::TempPath::CreateTempDir("fl_model_telemetry_");
  WebService service(catalog, catalog, fl::test::NullLog(), cache.string(), bindings.model_load_manager, sessions,
                     telemetry, []() {});
  const auto urls = service.Start({"http://127.0.0.1:0"});
  ASSERT_EQ(urls.size(), 1u);
  httplib::Client client(urls[0]);
  client.set_read_timeout(10, 0);

  const auto load_response = client.Get("/models/load/alpha-model");
  ASSERT_TRUE(load_response);
  EXPECT_EQ(load_response->status, 400);
  const auto unload_response = client.Get("/models/unload/alpha-model");
  ASSERT_TRUE(unload_response);
  EXPECT_EQ(unload_response->status, 200);
  service.Stop();

  const auto events = telemetry.Events();
  ASSERT_EQ(events.actions.size(), 2u);
  EXPECT_EQ(events.actions[0].action, Action::kModelLoad);
  EXPECT_EQ(events.actions[0].status, ActionStatus::kClientError);
  EXPECT_EQ(events.actions[0].model_id, "alpha-model:1");
  EXPECT_EQ(events.actions[1].action, Action::kModelUnload);
  EXPECT_EQ(events.actions[1].status, ActionStatus::kSkipped);
  EXPECT_EQ(events.actions[1].model_id, "alpha-model:1");
}

INSTANTIATE_TEST_SUITE_P(
    RejectedRequests, WebServiceTelemetryTest,
    ::testing::Values(
        RouteTelemetryCase{"POST", "/v1/chat/completions", "", Action::kOpenAIChatCompletions, 400},
        RouteTelemetryCase{"POST", "/v1/chat/completions", "{", Action::kOpenAIChatCompletions, 400},
        RouteTelemetryCase{"POST", "/v1/audio/transcriptions", "", Action::kOpenAIAudioTranscribe, 400},
        RouteTelemetryCase{"POST", "/v1/embeddings", "", Action::kOpenAIEmbeddings, 400},
        RouteTelemetryCase{"POST", "/v1/responses", "", Action::kOpenAIResponsesCreate, 400},
        RouteTelemetryCase{"GET", "/v1/responses/missing", "", Action::kOpenAIResponsesGet, 404},
        RouteTelemetryCase{"GET", "/v1/responses/missing/input_items", "", Action::kOpenAIResponsesGetInputItems, 404},
        RouteTelemetryCase{"DELETE", "/v1/responses/missing", "", Action::kOpenAIResponsesDelete, 404},
        RouteTelemetryCase{"GET", "/v1/models/missing", "", Action::kOpenAIModelRetrieve, 404}));

TEST(WebServiceTelemetryStatusTest, HttpStatusMapsWithoutMaskingFailures) {
  EXPECT_EQ(ResponseToActionStatus(nullptr), ActionStatus::kFailure);
  EXPECT_EQ(ResponseToActionStatus(JsonResponse(Status::CODE_200, json::object())), ActionStatus::kSuccess);
  EXPECT_EQ(ResponseToActionStatus(JsonResponse(Status::CODE_400, json::object())), ActionStatus::kClientError);
  EXPECT_EQ(ResponseToActionStatus(JsonResponse(Status::CODE_404, json::object())), ActionStatus::kClientError);
  EXPECT_EQ(ResponseToActionStatus(JsonResponse(Status::CODE_408, json::object())), ActionStatus::kTimeout);
  EXPECT_EQ(ResponseToActionStatus(JsonResponse(Status::CODE_504, json::object())), ActionStatus::kTimeout);
  EXPECT_EQ(ResponseToActionStatus(JsonResponse(Status::CODE_500, json::object())), ActionStatus::kFailure);
}

class WebServiceTelemetryInferenceTest : public ::testing::TestWithParam<std::tuple<bool, bool>> {};

TEST_P(WebServiceTelemetryInferenceTest, RouteAndNestedInferenceShareOneOperationContext) {
  test::CpuOnlyEpDetector ep_detector;
  ModelLoadManager load_manager(ep_detector, fl::test::NullLog());
  SessionManager sessions(fl::test::NullLog());
  WebUsageTelemetry telemetry;
  test::FakeServiceBindings bindings;
  test::MockCatalog catalog;
  const auto model_path = test::GetTestModelPath(test::kTestChatModelAlias);
  auto loaded = load_manager.LoadModel(model_path.string(), "telemetry-chat");
  ASSERT_EQ(loaded.status, ModelLoadManager::LoadStatus::kSuccess);

  ModelInfo model_info;
  model_info.model_id = "telemetry-chat";
  model_info.name = "telemetry-chat";
  model_info.task = "chat-completion";
  catalog.AddModel(Model::FromModelInfo(std::move(model_info), model_path.string(),
                                        bindings.download_manager, load_manager));
  auto cache = test::TempPath::CreateTempDir("fl_inference_telemetry_");
  WebService service(catalog, catalog, fl::test::NullLog(), cache.string(), load_manager, sessions, telemetry,
                     []() {});
  const auto urls = service.Start({"http://127.0.0.1:0"});
  ASSERT_EQ(urls.size(), 1u);
  httplib::Client client(urls[0]);
  client.set_read_timeout(60, 0);
  const auto [streaming, responses] = GetParam();
  const auto route_action = responses ? Action::kOpenAIResponsesCreate : Action::kOpenAIChatCompletions;
  json request = {
      {"model", "telemetry-chat"},
      {"stream", streaming},
  };
  if (responses) {
    request["input"] = "Say hello.";
    request["max_output_tokens"] = 2;
  } else {
    request["messages"] = {{{"role", "user"}, {"content", "Say hello."}}};
    request["max_tokens"] = 2;
  }

  const auto response = client.Post(responses ? "/v1/responses" : "/v1/chat/completions",
                                    {{"User-Agent", "telemetry-test-client"}},
                                    request.dump(), "application/json");
  ASSERT_TRUE(response) << httplib::to_string(response.error());
  EXPECT_EQ(response->status, 200) << response->body;
  if (streaming) {
    EXPECT_NE(response->body.find("data: [DONE]"), std::string::npos);
  }

  service.Stop();  // Joins the stream before inspecting its terminal route action.
  const auto events = telemetry.Events();
  ASSERT_EQ(events.actions.size(), 3u);
  ASSERT_EQ(events.models.size(), 1u);
  const auto& usage = events.models[0];
  EXPECT_EQ(usage.model_id, "telemetry-chat");
  EXPECT_EQ(usage.execution_provider, "CPUExecutionProvider");
  EXPECT_EQ(usage.user_agent, "unknown-http-client");
  EXPECT_EQ(usage.num_messages, 1u);
  EXPECT_EQ(usage.stream, streaming);
  EXPECT_TRUE(usage.indirect);
  EXPECT_EQ(usage.correlation_id.size(), 36u);
  std::vector<Action> action_ids;
  for (const auto& action : events.actions) {
    action_ids.push_back(action.action);
    EXPECT_EQ(action.status, ActionStatus::kSuccess);
    EXPECT_EQ(action.model_id, "telemetry-chat");
    EXPECT_EQ(action.context.correlation_id, usage.correlation_id);
    EXPECT_EQ(action.context.user_agent, "unknown-http-client");
    EXPECT_EQ(action.context.indirect, action.action != route_action);
  }

  EXPECT_EQ(action_ids, (std::vector<Action>{Action::kSessionCreate, Action::kSessionProcessRequest,
                                             route_action}));
}

INSTANTIATE_TEST_SUITE_P(StreamingAndNonStreaming, WebServiceTelemetryInferenceTest,
                         ::testing::Combine(::testing::Bool(), ::testing::Bool()));

#endif  // FOUNDRY_LOCAL_HAS_WEB_SERVICE
