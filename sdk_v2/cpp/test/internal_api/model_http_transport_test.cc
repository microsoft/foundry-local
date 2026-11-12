// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "http/model_http_transport.h"

#include "c_api_types.h"
#include "catalog/azure_catalog_client.h"
#include "configuration.h"
#include "download/blob_downloader.h"
#include "download/download_manager.h"
#include "exception.h"
#include "telemetry/telemetry_logger.h"
#include "test_helpers.h"

#if FOUNDRY_LOCAL_HAS_SHARED_HOST
#include "hosting/host_profile.h"
#endif

#include <azure/core/http/transport.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fl {
namespace {

using namespace std::chrono_literals;

struct FakeTransport {
  struct Operation {
    FakeTransport& owner;
    const flHttpRequestData& request;
    std::string body;
    size_t position = 0;
    std::vector<std::pair<std::string, std::string>> header_storage;
    std::vector<flHttpHeader> headers;
    std::mutex mutex;
    std::condition_variable wake;
    bool cancelled = false;
    uint32_t response_status = 200;
  };

  enum class Invalid { None,
                       NullOperation,
                       Version,
                       Status,
                       Headers,
                       HeaderName,
                       ReadCount };
  enum class Error { None,
                     Create,
                     Send,
                     Read };

  std::atomic<int> retains{0};
  std::atomic<int> releases{0};
  std::atomic<int> created{0};
  std::atomic<int> sent{0};
  std::atomic<int> reads{0};
  std::atomic<int> cancelled{0};
  std::atomic<int> operations_released{0};
  std::atomic<bool> entered{false};
  bool block_send = false;
  bool block_read = false;
  bool acquisition = false;
  bool omit_headers = false;
  Invalid invalid = Invalid::None;
  Error error = Error::None;
  int transient_sends = 0;
  std::string body = "abcdef";
  uint32_t status = 200;
  size_t short_read = 3;
  std::string method;
  std::string url;
  std::string request_body;
  std::map<std::string, std::string> request_headers;
  uint64_t timeout_ms = 0;
  bool saw_range = false;
  bool saw_md5 = false;
  bool saw_if_match = false;

  static flStatus* Failure() noexcept {
    return FoundryLocalGetApi(4)->Status_Create(FOUNDRY_LOCAL_ERROR_NETWORK, "secret SAS must not escape");
  }

  static void FL_API_CALL Retain(void* data) noexcept { ++static_cast<FakeTransport*>(data)->retains; }
  static void FL_API_CALL Release(void* data) noexcept { ++static_cast<FakeTransport*>(data)->releases; }

  static flStatus* FL_API_CALL Create(void* data, const flHttpRequestData* request, void** output) noexcept {
    auto& owner = *static_cast<FakeTransport*>(data);
    ++owner.created;
    if (owner.error == Error::Create) {
      return Failure();
    }

    if (owner.invalid == Invalid::NullOperation) {
      return nullptr;
    }

    *output = new Operation{owner, *request, owner.body, 0, {}, {}, {}, {}, false};
    return nullptr;
  }

  static void AcquisitionResponse(Operation& operation) {
    auto& owner = operation.owner;
    const auto& request = operation.request;
    const auto add = [&](std::string name, std::string value) {
      operation.header_storage.emplace_back(std::move(name), std::move(value));
    };
    if (std::string(request.url).find("modelregistry") != std::string::npos) {
      operation.body = R"({"blobSasUri":"https://test.blob.core.windows.net/container?sig=test"})";
    } else if (std::string(request.url).find("comp=list") != std::string::npos) {
      operation.body =
          "<EnumerationResults ServiceEndpoint=\"https://test.blob.core.windows.net/\" ContainerName=\"container\">"
          "<Blobs><Blob><Name>blob.bin</Name><Properties>"
          "<Last-Modified>Wed, 14 Oct 2026 00:00:00 GMT</Last-Modified><Etag>test-etag</Etag>"
          "<Content-Length>32</Content-Length><BlobType>BlockBlob</BlobType></Properties></Blob></Blobs>"
          "<NextMarker /></EnumerationResults>";
      add("content-type", "application/xml");
    } else {
      const bool head = std::string(request.method) == "HEAD";
      operation.response_status = head ? 200 : 206;
      operation.body = head ? "" : std::string(32, '*');
      add("etag", "\"test-etag\"");
      add("last-modified", "Wed, 14 Oct 2026 00:00:00 GMT");
      add("x-ms-creation-time", "Wed, 14 Oct 2026 00:00:00 GMT");
      add("x-ms-server-encrypted", "true");
      add("x-ms-blob-type", "BlockBlob");
      add("content-length", "32");
      if (!head) {
        add("content-range", "bytes 0-31/32");
        add("content-md5", "SaaDheEjzuynJH8eW6AEpQ==");
        for (size_t i = 0; i < request.header_count; ++i) {
          const auto& header = request.headers[i];
          owner.saw_range |= std::string(header.name) == "x-ms-range" && std::string(header.value) == "bytes=0-31";
          owner.saw_md5 |= std::string(header.name) == "x-ms-range-get-content-md5" &&
                           std::string(header.value) == "true";
          owner.saw_if_match |= std::string(header.name) == "if-match" &&
                                std::string(header.value) == "\"test-etag\"";
        }
      }

      return;
    }

    add("content-length", std::to_string(operation.body.size()));
  }

  static bool Wait(Operation& operation, bool block) {
    std::unique_lock lock(operation.mutex);
    if (block) {
      operation.owner.entered = true;
      operation.wake.wait_for(lock, 3s, [&] { return operation.cancelled; });
    }

    return operation.cancelled;
  }

  static flStatus* FL_API_CALL Send(void* data, flHttpResponseData* response) noexcept {
    auto& operation = *static_cast<Operation*>(data);
    auto& owner = operation.owner;
    ++owner.sent;
    owner.method = operation.request.method;
    owner.url = operation.request.url;
    owner.request_headers.clear();
    for (size_t i = 0; i < operation.request.header_count; ++i) {
      const auto& header = operation.request.headers[i];
      owner.request_headers[header.name] = header.value;
    }

    if (operation.request.body_size != 0) {
      owner.request_body.assign(reinterpret_cast<const char*>(operation.request.body), operation.request.body_size);
    }
    owner.timeout_ms = operation.request.timeout_ms;
    operation.response_status = owner.status;
    if (Wait(operation, owner.block_send)) {
      return FoundryLocalGetApi(4)->Status_Create(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "cancelled");
    }

    if (owner.error == Error::Send) {
      return Failure();
    }

    if (owner.transient_sends > 0) {
      --owner.transient_sends;
      return Failure();
    }

    if (owner.acquisition) {
      AcquisitionResponse(operation);
    } else if (!owner.omit_headers) {
      operation.header_storage = {{"X-Model", "first"}, {"X-Model", "last"}};
    }

    for (const auto& [name, value] : operation.header_storage) {
      operation.headers.push_back({name.c_str(), value.c_str()});
    }

    EXPECT_EQ(response->version, FOUNDRY_LOCAL_HTTP_TRANSPORT_VERSION);
    response->status_code = operation.response_status;
    response->headers = operation.headers.data();
    response->header_count = operation.headers.size();
    if (owner.invalid == Invalid::Version) {
      response->version = 100;
    } else if (owner.invalid == Invalid::Status) {
      response->status_code = 0;
    } else if (owner.invalid == Invalid::Headers) {
      response->headers = nullptr;
      response->header_count = 1;
    } else if (owner.invalid == Invalid::HeaderName) {
      operation.headers.front().name = nullptr;
    }

    return nullptr;
  }

  static flStatus* FL_API_CALL Read(void* data, uint8_t* buffer, size_t capacity, size_t* count) noexcept {
    auto& operation = *static_cast<Operation*>(data);
    auto& owner = operation.owner;
    ++owner.reads;
    if (Wait(operation, owner.block_read)) {
      return FoundryLocalGetApi(4)->Status_Create(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "cancelled");
    }

    if (owner.error == Error::Read) {
      return Failure();
    }

    const auto read = (std::min)({capacity, owner.short_read, operation.body.size() - operation.position});
    std::memcpy(buffer, operation.body.data() + operation.position, read);
    operation.position += read;
    *count = owner.invalid == Invalid::ReadCount ? capacity + 1 : read;
    return nullptr;
  }

  static void FL_API_CALL Cancel(void* data) noexcept {
    auto& operation = *static_cast<Operation*>(data);
    std::lock_guard lock(operation.mutex);
    operation.cancelled = true;
    ++operation.owner.cancelled;
    operation.wake.notify_all();
  }

  static void FL_API_CALL ReleaseOperation(void* data) noexcept {
    auto* operation = static_cast<Operation*>(data);
    // The request storage must still be readable when the adapter releases its operation.
    EXPECT_EQ(operation->request.version, FOUNDRY_LOCAL_HTTP_TRANSPORT_VERSION);
    EXPECT_NE(operation->request.url, nullptr);
    ++operation->owner.operations_released;
    delete operation;
  }

  flHttpTransport Table() {
    return {FOUNDRY_LOCAL_HTTP_TRANSPORT_VERSION, this, Retain, Release, Create, Send, Read, Cancel, ReleaseOperation};
  }

  std::shared_ptr<http::ModelHttpTransport> Make() { return std::make_shared<http::ModelHttpTransport>(Table()); }
};

class CpuDetector final : public IEpDetector {
 public:
  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    return {{"CPU", {"CPUExecutionProvider"}}};
  }
};

TEST(ModelHttpTransportTest, VersionFourAddsSetterWithoutChangingOlderNegotiation) {
  EXPECT_EQ(FOUNDRY_LOCAL_API_VERSION, 4);
  const auto* old = FoundryLocalGetApi(3);
  const auto* current = FoundryLocalGetApi(4);
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(FoundryLocalGetApi(0), old);
  EXPECT_EQ(FoundryLocalGetApi(1), old);
  EXPECT_EQ(FoundryLocalGetApi(2), old);
  EXPECT_EQ(old->GetConfigurationApi()->SetModelHttpTransport, nullptr);
  EXPECT_NE(current->GetConfigurationApi()->SetModelHttpTransport, nullptr);
  EXPECT_EQ(current->Manager_Create, old->Manager_Create);
  EXPECT_EQ(FoundryLocalGetApi(5), nullptr);
  EXPECT_EQ(offsetof(flConfigurationApi, SetModelHttpTransport), 12 * sizeof(void*));
}

TEST(ModelHttpTransportTest, SetterCopiesTableAndSnapshotsRetainOldContextAfterReplacementAndClear) {
  const auto& api = *FoundryLocalGetApi(4);
  const auto& configuration = *api.GetConfigurationApi();
  flConfiguration* config = nullptr;
  ASSERT_EQ(configuration.Create("transport-test", &config), nullptr);
  FakeTransport first;
  FakeTransport second;
  auto table = first.Table();
  ASSERT_EQ(configuration.SetModelHttpTransport(config, &table), nullptr);
  auto snapshot = *AsImpl(config);
  table.Send = nullptr;
  EXPECT_EQ(snapshot.model_http_transport->Request("GET", "https://offline.invalid").body, "abcdef");
  auto replacement = second.Table();
  ASSERT_EQ(configuration.SetModelHttpTransport(config, &replacement), nullptr);
  EXPECT_EQ(first.releases, 0);
  ASSERT_EQ(configuration.SetModelHttpTransport(config, nullptr), nullptr);
  EXPECT_EQ(second.releases, 1);
  configuration.Configuration_Release(config);
  EXPECT_EQ(first.retains, 1);
  EXPECT_EQ(first.releases, 0);
  snapshot.model_http_transport.reset();
  EXPECT_EQ(first.releases, 1);
}

TEST(ModelHttpTransportTest, InvalidSetterLeavesExistingTransportUnchanged) {
  const auto& api = *FoundryLocalGetApi(4);
  const auto& configuration = *api.GetConfigurationApi();
  flConfiguration* config = nullptr;
  ASSERT_EQ(configuration.Create("transport-test", &config), nullptr);
  FakeTransport fake;
  auto table = fake.Table();
  ASSERT_EQ(configuration.SetModelHttpTransport(config, &table), nullptr);
  table.Read = nullptr;
  auto* error = configuration.SetModelHttpTransport(config, &table);
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(api.Status_GetErrorCode(error), FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT);
  api.Status_Release(error);
  EXPECT_EQ(fake.retains, 1);
  EXPECT_EQ(AsImpl(config)->model_http_transport->Request("GET", "https://offline.invalid").body, "abcdef");
  configuration.Configuration_Release(config);
  EXPECT_EQ(fake.releases, 1);
  error = configuration.SetModelHttpTransport(nullptr, nullptr);
  ASSERT_NE(error, nullptr);
  api.Status_Release(error);
}

TEST(ModelHttpTransportTest, AllCallbacksAndExactTransportVersionAreRequiredBeforeRetain) {
  FakeTransport fake;
  for (int index = 0; index != 8; ++index) {
    auto table = fake.Table();
    switch (index) {
      case 0:
        table.version = 2;
        break;
      case 1:
        table.Retain = nullptr;
        break;
      case 2:
        table.Release = nullptr;
        break;
      case 3:
        table.CreateOperation = nullptr;
        break;
      case 4:
        table.Send = nullptr;
        break;
      case 5:
        table.Read = nullptr;
        break;
      case 6:
        table.Cancel = nullptr;
        break;
      case 7:
        table.ReleaseOperation = nullptr;
        break;
    }

    EXPECT_THROW(http::ModelHttpTransport{table}, Exception);
  }

  EXPECT_EQ(fake.retains, 0);
}

TEST(ModelHttpTransportTest, ShortReadsPreserveHttpErrorsAndCopiedResponseHeaders) {
  FakeTransport fake;
  fake.status = 403;
  const auto response = fake.Make()->Request("POST", "https://offline.invalid", "{}");
  EXPECT_EQ(response.status, 403);
  EXPECT_EQ(response.body, "abcdef");
  EXPECT_EQ(response.headers.at("x-model"), "last");
  EXPECT_EQ(fake.method, "POST");
  EXPECT_EQ(fake.request_body, "{}");
  EXPECT_EQ(fake.timeout_ms, 30000);
  EXPECT_EQ(fake.request_headers.at("content-type"), "application/json");
  EXPECT_EQ(fake.sent, 1);
  EXPECT_EQ(fake.reads, 3);
  EXPECT_EQ(fake.operations_released, 1);
  EXPECT_EQ(fake.releases, 1);
}

TEST(ModelHttpTransportTest, EmptyBodyAndZeroHeadersAreValidAndZeroTimeoutMeansNoDeadline) {
  FakeTransport fake;
  fake.status = 204;
  fake.body.clear();
  fake.omit_headers = true;
  http::HttpRequestOptions options;
  options.timeout = 0ms;
  const auto response = fake.Make()->Request("GET", "https://offline.invalid", {}, options);
  EXPECT_EQ(response.status, 204);
  EXPECT_EQ(response.body, "");
  EXPECT_TRUE(response.headers.empty());
  EXPECT_EQ(fake.timeout_ms, 0);
  EXPECT_EQ(fake.reads, 1);
  EXPECT_EQ(fake.operations_released, 1);
}

TEST(ModelHttpTransportTest, InvalidCallbackOutputsFailWithoutFallbackAndReleaseOperations) {
  for (const auto invalid : {FakeTransport::Invalid::NullOperation, FakeTransport::Invalid::Version,
                             FakeTransport::Invalid::Status, FakeTransport::Invalid::Headers,
                             FakeTransport::Invalid::HeaderName, FakeTransport::Invalid::ReadCount}) {
    SCOPED_TRACE(static_cast<int>(invalid));
    FakeTransport fake;
    fake.invalid = invalid;
    EXPECT_THROW(fake.Make()->Request("GET", "https://offline.invalid"), Exception);
    EXPECT_EQ(fake.created, 1);
    EXPECT_EQ(fake.operations_released, invalid == FakeTransport::Invalid::NullOperation ? 0 : 1);
    EXPECT_EQ(fake.releases, 1);
  }
}

TEST(ModelHttpTransportTest, ErrorStatusesTransferOwnershipAndNeverExposeSignedUrls) {
  for (const auto phase : {FakeTransport::Error::Create, FakeTransport::Error::Send, FakeTransport::Error::Read}) {
    FakeTransport fake;
    fake.error = phase;
    const auto response = fake.Make()->Request("GET", "https://offline.invalid?sig=secret");
    EXPECT_EQ(response.status, 0);
    EXPECT_EQ(response.body.find("secret"), std::string::npos);

    EXPECT_EQ(fake.operations_released, phase == FakeTransport::Error::Create ? 0 : 1);
    EXPECT_EQ(fake.releases, 1);
  }
}

TEST(ModelHttpTransportTest, AzureSendAndReadErrorsUseTransportExceptionsForExistingAzurePolicies) {
  for (const auto phase : {FakeTransport::Error::Create, FakeTransport::Error::Send, FakeTransport::Error::Read}) {
    FakeTransport fake;
    fake.error = phase;
    auto transport = fake.Make()->AzureTransport();
    Azure::Core::Http::Request request(Azure::Core::Http::HttpMethod::Get,
                                       Azure::Core::Url("https://offline.invalid"));
    std::array<uint8_t, 4> buffer{};
    EXPECT_THROW(
        {
          auto response = transport->Send(request, {});
          response->ExtractBodyStream()->Read(buffer.data(), buffer.size());
        },
        Azure::Core::Http::TransportException);
    EXPECT_EQ(fake.operations_released, phase == FakeTransport::Error::Create ? 0 : 1);
  }
}

#if FOUNDRY_LOCAL_HAS_SHARED_HOST
TEST(ModelHttpTransportTest, SharedHostProfileRetainsClientTransportWithoutSerializingItOrChangingCompatibility) {
  FakeTransport fake;
  const auto directory = test::TempPath::CreateTempDir("foundry-custom-profile-");
  Configuration config;
  config.app_name = "transport-test";
  config.app_data_dir = (directory.path() / "data").string();
  config.model_cache_dir = (directory.path() / "models").string();
  config.logs_dir = (directory.path() / "logs").string();
  const hosting::RuntimeIdentity runtime{"native-build", "linux-x64", 1};
  const auto builtin = hosting::BuildHostProfile(config, runtime);
  config.model_http_transport = fake.Make();
  auto custom = hosting::BuildHostProfile(config, runtime);
  EXPECT_EQ(custom.key, builtin.key);
  EXPECT_EQ(custom.compatibility, builtin.compatibility);
  EXPECT_EQ(custom.configuration.model_http_transport, config.model_http_transport);
  EXPECT_FALSE(config.catalog_region.has_value());
  config.model_http_transport.reset();
  EXPECT_EQ(fake.releases, 0);
  EXPECT_EQ(custom.configuration.model_http_transport->Request("GET", "https://offline.invalid").body, "abcdef");
  custom.configuration.model_http_transport.reset();
  EXPECT_EQ(fake.releases, 1);
}
#endif

TEST(ModelHttpTransportTest, BufferedMetadataRejectsOversizeResponses) {
  FakeTransport fake;
  fake.body.assign(16 * 1024 * 1024 + 1, 'x');
  fake.short_read = 64 * 1024;
  EXPECT_THROW(fake.Make()->Request("GET", "https://offline.invalid"), Exception);
  EXPECT_EQ(fake.operations_released, 1);
}

TEST(ModelHttpTransportTest, AzureBodyStreamsShortReadsAndRetainsContextUntilResponseIsDestroyed) {
  FakeTransport fake;
  auto transport = fake.Make()->AzureTransport();
  Azure::Core::Http::Request request(Azure::Core::Http::HttpMethod::Get, Azure::Core::Url("https://offline.invalid"));
  auto response = transport->Send(request, {});
  EXPECT_EQ(fake.reads, 0);
  EXPECT_EQ(fake.timeout_ms, 0);
  transport.reset();
  EXPECT_EQ(fake.releases, 0);
  auto body = response->ExtractBodyStream();
  response.reset();
  std::array<uint8_t, 4> buffer{};
  EXPECT_EQ(body->Read(buffer.data(), buffer.size()), 3);
  EXPECT_EQ(std::string(buffer.begin(), buffer.begin() + 3), "abc");
  EXPECT_EQ(body->Read(buffer.data(), buffer.size()), 3);
  EXPECT_EQ(std::string(buffer.begin(), buffer.begin() + 3), "def");
  EXPECT_EQ(body->Read(buffer.data(), buffer.size()), 0);
  body.reset();
  EXPECT_EQ(fake.operations_released, 1);
  EXPECT_EQ(fake.releases, 1);
}

TEST(ModelHttpTransportTest, PreSendCancellationCallsCancelAndDoesNotSend) {
  FakeTransport fake;
  auto transport = fake.Make()->AzureTransport();
  Azure::Core::Context context;
  context.Cancel();
  Azure::Core::Http::Request request(Azure::Core::Http::HttpMethod::Get, Azure::Core::Url("https://offline.invalid"));
  EXPECT_THROW(transport->Send(request, context), Azure::Core::OperationCancelledException);
  EXPECT_EQ(fake.sent, 0);
  EXPECT_EQ(fake.cancelled, 1);
  EXPECT_EQ(fake.operations_released, 1);
}

void VerifyBlockedCancellation(bool send) {
  FakeTransport fake;
  fake.block_send = send;
  fake.block_read = !send;
  auto transport = fake.Make()->AzureTransport();
  Azure::Core::Context context;
  Azure::Core::Http::Request request(Azure::Core::Http::HttpMethod::Get, Azure::Core::Url("https://offline.invalid"));
  auto work = std::async(std::launch::async, [&] {
    try {
      auto response = transport->Send(request, send ? context : Azure::Core::Context{});
      std::array<uint8_t, 8> buffer{};
      response->ExtractBodyStream()->Read(buffer.data(), buffer.size(), context);
    } catch (const Azure::Core::OperationCancelledException&) {
      return true;
    } catch (const std::exception&) {
      return false;
    }

    return false;
  });
  const auto limit = std::chrono::steady_clock::now() + 1s;
  while (!fake.entered && std::chrono::steady_clock::now() < limit) {
    std::this_thread::sleep_for(1ms);
  }

  ASSERT_TRUE(fake.entered);
  context.Cancel();
  EXPECT_EQ(work.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(work.get());
  EXPECT_EQ(fake.cancelled, 1);
  EXPECT_EQ(fake.operations_released, 1);
}

TEST(ModelHttpTransportTest, ContextCancellationInterruptsBlockedSendAndDrainsBeforeRelease) {
  VerifyBlockedCancellation(true);
}

TEST(ModelHttpTransportTest, ContextCancellationInterruptsBlockedReadAndDrainsBeforeRelease) {
  VerifyBlockedCancellation(false);
}

TEST(ModelHttpTransportTest, TotalTimeoutInterruptsBlockedSendWithoutRestartingForBody) {
  FakeTransport fake;
  fake.block_send = true;
  http::HttpRequestOptions options;
  options.timeout = 20ms;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(fake.Make()->Request("GET", "https://offline.invalid", {}, options).status, 0);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  EXPECT_EQ(fake.cancelled, 1);
  EXPECT_EQ(fake.operations_released, 1);
}

TEST(ModelHttpTransportTest, TotalTimeoutAlsoInterruptsBlockedRead) {
  FakeTransport fake;
  fake.block_read = true;
  http::HttpRequestOptions options;
  options.timeout = 20ms;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(fake.Make()->Request("GET", "https://offline.invalid", {}, options).status, 0);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  EXPECT_EQ(fake.cancelled, 1);
  EXPECT_EQ(fake.operations_released, 1);
}

TEST(ModelHttpTransportTest, DeadlineCancellationIsStickyAcrossIdleTimeBeforeRead) {
  FakeTransport fake;
  auto transport = fake.Make()->AzureTransport();
  const auto context = Azure::Core::Context{}.WithDeadline(Azure::DateTime(std::chrono::system_clock::now() + 20ms));
  Azure::Core::Http::Request request(Azure::Core::Http::HttpMethod::Get,
                                     Azure::Core::Url("https://offline.invalid"));
  auto response = transport->Send(request, context);
  const auto limit = std::chrono::steady_clock::now() + 1s;
  while (fake.cancelled == 0 && std::chrono::steady_clock::now() < limit) {
    std::this_thread::sleep_for(1ms);
  }

  ASSERT_EQ(fake.cancelled, 1);
  std::array<uint8_t, 4> buffer{};
  EXPECT_THROW(response->ExtractBodyStream()->Read(buffer.data(), buffer.size()),
               Azure::Core::OperationCancelledException);
  EXPECT_EQ(fake.reads, 0);
  EXPECT_EQ(fake.operations_released, 1);
}

TEST(ModelHttpTransportTest, ExternalDownloadCancellationInterruptsBlockedInitialHead) {
  FakeTransport fake;
  fake.acquisition = true;
  fake.block_send = true;
  AzureBlobDownloader downloader(test::NullLog(), fake.Make()->AzureTransport());
  const auto directory = test::TempPath::CreateTempDir("foundry-custom-cancel-");
  std::atomic<bool> cancelled{false};
  auto work = std::async(std::launch::async, [&] {
    try {
      downloader.DownloadBlob("https://test.blob.core.windows.net/container?sig=test", "blob.bin",
                              (directory.path() / "blob.bin").string(), 1, nullptr, &cancelled);
    } catch (const Exception& error) {
      return error.code();
    } catch (const std::exception&) {
      return FOUNDRY_LOCAL_ERROR_INTERNAL;
    }

    return FOUNDRY_LOCAL_OK;
  });
  const auto limit = std::chrono::steady_clock::now() + 1s;
  while (!fake.entered && std::chrono::steady_clock::now() < limit) {
    std::this_thread::sleep_for(1ms);
  }

  ASSERT_TRUE(fake.entered);
  cancelled = true;
  EXPECT_EQ(work.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(work.get(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  EXPECT_EQ(fake.method, "HEAD");
  EXPECT_EQ(fake.cancelled, 1);
  EXPECT_EQ(fake.operations_released, 1);
}

TEST(ModelHttpTransportTest, CatalogPostUsesCustomTransportAndParsesEmptyCatalog) {
  FakeTransport fake;
  fake.body = R"({"value":[]})";
  const auto transport = fake.Make();
  CpuDetector detector;
  AzureCatalogClient client("https://offline.invalid/catalog", "", detector, test::NullLog(),
                            {}, {}, transport);
  EXPECT_TRUE(client.FetchAllModels().empty());
  EXPECT_EQ(fake.method, "POST");
  EXPECT_EQ(fake.url, "https://offline.invalid/catalog");
  EXPECT_EQ(fake.request_headers.at("user-agent"), "AzureAiStudio");
  EXPECT_EQ(fake.request_headers.at("x-ms-use-full-service-contracts"), "true");
  EXPECT_NE(fake.request_body.find("filters"), std::string::npos);
}

TEST(ModelHttpTransportTest, CatalogRetryPolicyRecreatesCustomOperationsWithoutBuiltinFallback) {
  FakeTransport fake;
  fake.body = R"({"value":[]})";
  fake.transient_sends = 1;
  CpuDetector detector;
  AzureCatalogClient client("https://offline.invalid/catalog", "", detector, test::NullLog(), {},
                            {.max_retries = 1, .base_delay = 0ms, .max_total = 1s}, fake.Make());
  EXPECT_TRUE(client.FetchAllModels().empty());
  EXPECT_EQ(fake.created, 2);
  EXPECT_EQ(fake.sent, 2);
  EXPECT_EQ(fake.operations_released, 2);
}

TEST(ModelHttpTransportTest, DownloadManagerRoutesRegistryAndBlobListingHeadAndRangeThroughCustomTransport) {
  FakeTransport fake;
  fake.acquisition = true;
  const auto directory = test::TempPath::CreateTempDir("foundry-custom-transport-");
  TelemetryLogger telemetry("custom-transport-test", test::NullLog());
  DownloadManager manager(directory.string(), "centralus", 1, test::NullLog(), telemetry, true,
                          nullptr, nullptr, fake.Make());
  ModelInfo info;
  info.model_id = "transport-model:1";
  info.name = "transport-model";
  info.alias = "transport-model";
  info.version = 1;
  info.uri = "azureml://registries/test/models/model/versions/1";
  const auto model_path = manager.DownloadModel(info);
  std::ifstream file(std::filesystem::path(model_path) / "blob.bin", std::ios::binary);
  const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  EXPECT_EQ(contents, std::string(32, '*'));
  EXPECT_TRUE(fake.saw_range);
  EXPECT_TRUE(fake.saw_md5);
  EXPECT_TRUE(fake.saw_if_match);
  EXPECT_EQ(fake.created, fake.operations_released);
  EXPECT_GE(fake.created, 4);
}

}  // namespace
}  // namespace fl
