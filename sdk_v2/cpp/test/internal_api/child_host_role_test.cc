// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "c_api_types.h"
#include "exception.h"
#include "hosting/child_host_role.h"
#include "hosting/sdk_connection.h"
#include "internal_api/c_api_test_helpers.h"
#include "manager.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
#include <httplib.h>
#endif

namespace fl::hosting {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

TEST(ChildHostRoleTest, RejectsInvalidBootstrapFieldsInsteadOfTruncatingOrDefaulting) {
  const ChildHostRole role{"endpoint", std::string(64, 'A'), "incarnation", CurrentProcessIdentity()};
  const auto encoded = EncodeChildHostRole(role);
  const auto decoded = DecodeChildHostRole(encoded);
  EXPECT_EQ(decoded.endpoint, role.endpoint);
  EXPECT_EQ(decoded.profile_key, role.profile_key);
  EXPECT_EQ(decoded.incarnation, role.incarnation);
  EXPECT_EQ(decoded.starter, role.starter);

  const auto valid = Json::parse(encoded);
  for (const auto& patch : std::vector<Json>{
           {{"op", "replace"}, {"path", "/version"}, {"value", uint64_t{0x100000001}}},
           {{"op", "replace"}, {"path", "/starter/pid"}, {"value", -1}},
           {{"op", "replace"}, {"path", "/starter/generation"}, {"value", 0}},
           {{"op", "replace"}, {"path", "/profile"}, {"value", "wrong-profile"}},
           {{"op", "replace"}, {"path", "/incarnation"}, {"value", ""}},
           {{"op", "replace"}, {"path", "/endpoint"}, {"value", ""}},
           {{"op", "remove"}, {"path", "/starter"}},
       }) {
    EXPECT_THROW(DecodeChildHostRole(valid.patch(Json::array({patch})).dump()), Exception) << patch.dump();
  }

  EXPECT_THROW(DecodeChildHostRole("{"), Exception);
}

class ChildHostApiTest : public testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<uint64_t> sequence{0};
    const auto suffix = std::to_string(CurrentProcessIdentity().pid) + "-" + std::to_string(sequence.fetch_add(1));
    const auto scratch = std::filesystem::path(FOUNDRY_LOCAL_TEST_SCRATCH_DIR);
    ASSERT_TRUE(scratch.is_absolute());
    std::filesystem::create_directories(scratch);
    ASSERT_FALSE(std::filesystem::is_symlink(scratch));
    root_ = scratch / ("child-host-" + suffix);
    ASSERT_TRUE(std::filesystem::create_directory(root_));
    owns_root_ = true;
#ifdef _WIN32
    role_.endpoint = LR"(\\.\pipe\foundry-native-child-test-)" + std::filesystem::path(suffix).native();
#else
    std::filesystem::permissions(root_, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    socket_directory_ = test::MakePrivateIpcDirectory();
    role_.endpoint = socket_directory_ / "s";
#endif
    role_.profile_key = std::string(64, 'A');
    role_.incarnation = suffix;
    role_.starter = CurrentProcessIdentity();
    api_ = FoundryLocalGetApi(2);
    ASSERT_NE(api_, nullptr);
    const auto* configuration_api = api_->GetConfigurationApi();
    ASSERT_FL_OK(api_, configuration_api->Create("child-host-api-test", &config_));
    ASSERT_FL_OK(api_, configuration_api->SetAppDataDir(config_, (root_ / "data").string().c_str()));
    ASSERT_FL_OK(api_, configuration_api->SetModelCacheDir(config_, (root_ / "models").string().c_str()));
    ASSERT_FL_OK(api_, configuration_api->SetLogsDir(config_, (root_ / "logs").string().c_str()));
    AsImpl(config_)->disable_nonessential_telemetry = true;
  }

  void TearDown() override {
    api_->Manager_Release(manager_);
    api_->GetConfigurationApi()->Configuration_Release(config_);
    if (!socket_directory_.empty()) {
      std::filesystem::remove_all(socket_directory_);
    }
    if (owns_root_) {
      std::filesystem::remove_all(root_);
    }
  }

  void SetBootstrap(const std::string& encoded) {
    flKeyValuePairs* options = nullptr;
    api_->CreateKeyValuePairs(&options);
    api_->AddKeyValuePair(options, kChildHostOption, encoded.c_str());
    test::StatusGuard status{api_->GetConfigurationApi()->SetAdditionalOptions(config_, options), api_};
    api_->KeyValuePairs_Release(options);
    ASSERT_EQ(status.s, nullptr);
  }

  std::unique_ptr<SdkConnection> Attach() const {
    auto connection = std::make_unique<SdkConnection>(Connect(role_.endpoint), SdkConnection::Side::Client);
    const auto process = CurrentProcessIdentity();
    const auto result = connection->Call(Operation::Handshake,
                                         {{"protocol", SdkConnection::kProtocolVersion},
                                          {"profile", role_.profile_key},
                                          {"owner", true},
                                          {"process", {{"pid", process.pid}, {"generation", process.generation}}}});
    if (result.status != 0) {
      throw std::runtime_error(result.error);
    }

    return connection;
  }

  const flApi* api_ = nullptr;
  flConfiguration* config_ = nullptr;
  flManager* manager_ = nullptr;
  std::filesystem::path root_;
  std::filesystem::path socket_directory_;
  bool owns_root_ = false;
  ChildHostRole role_;
};

TEST_F(ChildHostApiTest, VersionedEntryPointOwnsTheServiceAndNormalFinalOwnerShutdown) {
  const auto phase = [](const char* description) {
    std::cout << "Child host lifecycle: " << description << std::endl;
  };
  phase("creating manager");
  SetBootstrap(EncodeChildHostRole(role_));
  ASSERT_FL_OK(api_, api_->Manager_Create(config_, &manager_));
  EXPECT_FALSE(Manager::Instance().GetConfiguration().additional_options.contains(kChildHostOption));
  EXPECT_FALSE(api_->Manager_IsShutdownRequested(manager_));
  phase("attaching owners");
  auto first = Attach();
  auto second = Attach();
  EXPECT_EQ(first->Call(Operation::Observe, {}).value.at("owners"), 2);

  {
    test::StatusGuard status{api_->Manager_Shutdown(manager_), api_};
    ASSERT_NE(status.s, nullptr);
    EXPECT_EQ(api_->Status_GetErrorCode(status.s), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
    EXPECT_FALSE(api_->Manager_IsShutdownRequested(manager_));
  }

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
  phase("checking HTTP shutdown rejection");
  const auto start = first->Call(Operation::WebServiceStart, {});
  ASSERT_EQ(start.status, 0) << start.error;
  const auto urls = first->Call(Operation::WebServiceUrls, {});
  ASSERT_EQ(urls.status, 0) << urls.error;
  httplib::Client http(urls.value.at(0).get<std::string>());
  http.set_read_timeout(5, 0);
  const auto shutdown = http.Post("/shutdown", "", "application/json");
  ASSERT_TRUE(shutdown);
  EXPECT_EQ(shutdown->status, 403);
  EXPECT_FALSE(api_->Manager_IsShutdownRequested(manager_));
#endif

  phase("closing owners");
  first->Close();
  EXPECT_EQ(second->Call(Operation::Observe, {}).status, 0);
  EXPECT_FALSE(api_->Manager_IsShutdownRequested(manager_));
  second->Close();
  phase("waiting for ownership-driven shutdown");
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!api_->Manager_IsShutdownRequested(manager_) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }

  ASSERT_TRUE(api_->Manager_IsShutdownRequested(manager_));
  phase("joining host shutdown");
  ASSERT_FL_OK(api_, api_->Manager_Shutdown(manager_));
  phase("host shutdown joined");
}

TEST_F(ChildHostApiTest, MalformedBootstrapDoesNotInitializeOrPoisonTheNativeSingleton) {
  SetBootstrap("{");
  {
    test::StatusGuard status{api_->Manager_Create(config_, &manager_), api_};
    ASSERT_NE(status.s, nullptr);
    EXPECT_EQ(api_->Status_GetErrorCode(status.s), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
    EXPECT_EQ(manager_, nullptr);
    EXPECT_THROW(Manager::Instance(), Exception);
  }

  SetBootstrap(EncodeChildHostRole(role_));
  ASSERT_FL_OK(api_, api_->Manager_Create(config_, &manager_));
  auto owner = Attach();
  EXPECT_EQ(owner->Call(Operation::Observe, {}).value.at("owners"), 1);
}

TEST_F(ChildHostApiTest, ChildRoleRejectsExternalServiceBeforeNativeInitialization) {
  SetBootstrap(EncodeChildHostRole(role_));
  ASSERT_FL_OK(api_, api_->GetConfigurationApi()->SetExternalServiceUrl(config_, "http://127.0.0.1:12345"));
  test::StatusGuard status{api_->Manager_Create(config_, &manager_), api_};
  ASSERT_NE(status.s, nullptr);
  EXPECT_EQ(api_->Status_GetErrorCode(status.s), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(manager_, nullptr);
  EXPECT_THROW(Manager::Instance(), Exception);
}

}  // namespace
}  // namespace fl::hosting
