// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for the download infrastructure:
// - ModelRegistryClient (SAS URI resolution)
// - DownloadBlobsToDirectory (blob filtering and orchestration)
// - InferenceModelWriter (inference_model.json)
// - FixVariantInferenceModelJson (variant fixup)
// - DownloadManager (full flow orchestration)
#include "catalog/azure_catalog_client.h"
#include "catalog/azure_catalog_models.h"
#include "catalog/local_model_scanner.h"
#include "download/blob_download_state.h"
#include "download/blob_downloader.h"
#include "platform/cross_process_file_lock.h"
#include "download/download_manager.h"
#include "download/inference_model_writer.h"
#include "download/model_registry_client.h"
#include "ep_detection/ep_detector.h"
#include "exception.h"
#include "logger.h"
#include "model.h"
#include "model_info.h"
#include "test_helpers.h"
#include "test_model_cache.h"
#include "util/path_safety.h"
#include "util/region_fallback.h"
#include <foundry_local/foundry_local_c.h>
#include <nlohmann/json.hpp>
#include <gtest/gtest.h>
#include <azure/core/cryptography/hash.hpp>
#include <azure/core/http/transport.hpp>
#include <azure/core/io/body_stream.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace fl;

// ========================================================================
// Test helpers
// ========================================================================

namespace {

using fl::test::TempPath;

/// Read entire file contents.
std::string ReadFile(const fs::path& path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::vector<uint8_t> Md5(std::string_view bytes) {
  Azure::Core::Cryptography::Md5Hash hash;
  return hash.Final(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

void DownloadBlobsToDirectoryForTest(IBlobDownloader& downloader,
                                     const std::string& sas_uri,
                                     const std::string& output_directory,
                                     const BlobDownloadOptions& options) {
  BlobDownloadStats stats;
  DownloadBlobsToDirectory(downloader, sas_uri, output_directory, options, stats, fl::test::NullLog());
}

class RecordingDownloadTelemetry final : public TelemetryLogger {
 public:
  RecordingDownloadTelemetry() : TelemetryLogger("download-test", fl::test::NullLog()) {}

  void RecordDownload(const DownloadInfo& info) override { calls.push_back(info); }

  std::vector<DownloadInfo> calls;
};

class CacheOperationLogger final : public ILogger {
 public:
  void Log(LogLevel, std::string_view message) override {
    if (on_log) {
      on_log(message);
    }
  }

  std::function<void(std::string_view)> on_log;
};

http::HttpResponse MakeRegistryResponse(std::string body, int status = 200) {
  http::HttpResponse response;
  response.status = status;
  response.body = std::move(body);
  return response;
}

/// Mock blob downloader for testing download orchestration.
class MockBlobDownloader : public IBlobDownloader {
 public:
  std::vector<BlobItemInfo> blobs_to_return;
  std::vector<std::string> downloaded_blobs;  // names of blobs that were "downloaded"
  std::string expected_sas_uri;
  std::string download_content;

  std::vector<BlobItemInfo> ListBlobs(const std::string& sas_uri) override {
    if (!expected_sas_uri.empty()) {
      EXPECT_EQ(sas_uri, expected_sas_uri);
    }
    return blobs_to_return;
  }

  void DownloadBlob(const std::string& /*sas_uri*/,
                    const std::string& blob_name,
                    const std::string& local_path,
                    int /*max_concurrency*/,
                    BlobBytesWrittenFn bytes_written_cb = nullptr,
                    std::atomic<bool>* /*cancelled*/ = nullptr) override {
    downloaded_blobs.push_back(blob_name);
    // Create a file so the test can verify it exists
    auto parent = fs::path(local_path).parent_path();
    if (!parent.empty()) {
      fs::create_directories(parent);
    }
    std::ofstream f(local_path);
    if (download_content.empty()) {
      f << "mock content for " << blob_name;
    } else {
      f << download_content;
    }

    // Report byte count for progress tracking (content_length from the matching blob)
    if (bytes_written_cb) {
      for (const auto& b : blobs_to_return) {
        if (b.name == blob_name) {
          bytes_written_cb(b.content_length);
          break;
        }
      }
    }
  }
};

/// Mock that performs basic downloads but lets progress-callback exceptions propagate.
/// Used to test that cancellation from the progress callback stops further blobs.
class CancellingMockDownloader : public IBlobDownloader {
 public:
  int download_count = 0;
  std::vector<BlobItemInfo> blobs_to_return;

  std::vector<BlobItemInfo> ListBlobs(const std::string&) override {
    return blobs_to_return;
  }

  void DownloadBlob(const std::string&, const std::string&,
                    const std::string& local_path, int,
                    BlobBytesWrittenFn bytes_written_cb,
                    std::atomic<bool>* /*cancelled*/) override {
    ++download_count;

    auto parent = fs::path(local_path).parent_path();
    if (!parent.empty()) {
      fs::create_directories(parent);
    }

    std::ofstream f(local_path);
    f << "content";

    // Report progress so the orchestrator's per-chunk callback fires.
    if (bytes_written_cb) {
      bytes_written_cb(100);
    }
  }
};

/// Mock that catches callback exceptions and then checks the cancelled flag,
/// simulating how AzureBlobDownloader detects cancellation between chunks.
class CancelCheckingMockDownloader : public IBlobDownloader {
 public:
  int download_count = 0;
  std::vector<BlobItemInfo> blobs_to_return;

  std::vector<BlobItemInfo> ListBlobs(const std::string&) override {
    return blobs_to_return;
  }

  void DownloadBlob(const std::string&, const std::string&,
                    const std::string& local_path, int,
                    BlobBytesWrittenFn bytes_written_cb,
                    std::atomic<bool>* cancelled) override {
    ++download_count;

    auto parent = fs::path(local_path).parent_path();
    if (!parent.empty()) {
      fs::create_directories(parent);
    }

    std::ofstream f(local_path);
    f << "content";

    // Call the progress callback, catching any cancellation exception
    // (simulating chunk-level error handling in a real downloader).
    if (bytes_written_cb) {
      try {
        bytes_written_cb(100);
      } catch (const fl::Exception&) {
        // Swallowed — real downloader might handle this per-chunk.
      }
    }

    // Check the cancelled flag as AzureBlobDownloader would between chunks.
    if (cancelled && cancelled->load(std::memory_order_relaxed)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "download cancelled");
    }
  }
};

class FailingTransferDownloader : public IBlobDownloader {
 public:
  std::vector<BlobItemInfo> ListBlobs(const std::string&) override { return {{"weights.bin", 100}}; }

  void DownloadBlob(const std::string&, const std::string&, const std::string&, int, BlobBytesWrittenFn,
                    std::atomic<bool>*) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    throw std::runtime_error("simulated transfer failure");
  }
};

/// EP detector returning all device types so the catalog returns the full model list.
class AllDevicesEpDetector : public IEpDetector {
 public:
  std::map<std::string, std::vector<std::string>> GetAvailableDevicesToEPs() const override {
    return {
        {"CPU", {"CPUExecutionProvider"}},
        {"GPU", {"CUDAExecutionProvider"}},
        {"NPU", {"QNNExecutionProvider"}},
    };
  }
};

}  // anonymous namespace

// ========================================================================
// ModelRegistryClient tests
// ========================================================================

TEST(ModelRegistryClientTest, ResolvesModelContainerFromJson) {
  ModelRegistryClient client("eastus", fl::test::NullLog(),
                             std::make_unique<RegionFallback>(fl::test::NullLog(), false),
                             [](const std::string& url) {
                               EXPECT_TRUE(url.find("assetId=") != std::string::npos);
                               return MakeRegistryResponse(R"({
      "blobSasUri": "https://storage.blob.core.windows.net/container?sv=2023-01-01&sig=abc",
      "modelEntity": {
        "description": "A test model"
      }
    })");
                             });

  auto container = client.ResolveModelContainer("azureml://registries/test/models/phi-3");
  EXPECT_EQ(container.blob_sas_uri,
            "https://storage.blob.core.windows.net/container?sv=2023-01-01&sig=abc");
  EXPECT_EQ(container.description, "A test model");
}

TEST(ModelRegistryClientTest, ThrowsOnEmptyAssetId) {
  ModelRegistryClient client("eastus", fl::test::NullLog(),
                             std::make_unique<RegionFallback>(fl::test::NullLog(), false));
  EXPECT_THROW(client.ResolveModelContainer(""), fl::Exception);
}

TEST(ModelRegistryClientTest, ThrowsNetworkOnHttpFailure) {
  ModelRegistryClient client("eastus", fl::test::NullLog(),
                             std::make_unique<RegionFallback>(fl::test::NullLog(), false),
                             [](const std::string&) {
                               return MakeRegistryResponse("upstream error", 500);
                             });
  try {
    client.ResolveModelContainer("azureml://test");
    FAIL() << "expected fl::Exception";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_NETWORK);
  }
}

TEST(ModelRegistryClientTest, ThrowsOnMissingSasUri) {
  ModelRegistryClient client("eastus", fl::test::NullLog(),
                             std::make_unique<RegionFallback>(fl::test::NullLog(), false),
                             [](const std::string&) {
                               return MakeRegistryResponse(R"({"modelEntity": {"description": "no sas uri"}})");
                             });
  // A 2xx response with a malformed/incomplete payload is a server-contract error, not a
  // transport failure — it stays INTERNAL.
  try {
    client.ResolveModelContainer("azureml://test");
    FAIL() << "expected fl::Exception";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_INTERNAL);
  }
}

TEST(ModelRegistryClientTest, ThrowsOnEmptyResponse) {
  ModelRegistryClient client("eastus", fl::test::NullLog(),
                             std::make_unique<RegionFallback>(fl::test::NullLog(), false),
                             [](const std::string&) { return MakeRegistryResponse(""); });
  EXPECT_THROW(client.ResolveModelContainer("azureml://test"), fl::Exception);
}

TEST(ModelRegistryClientTest, ThrowsOnMalformedJson) {
  ModelRegistryClient client("eastus", fl::test::NullLog(),
                             std::make_unique<RegionFallback>(fl::test::NullLog(), false),
                             [](const std::string&) { return MakeRegistryResponse("not json"); });
  EXPECT_THROW(client.ResolveModelContainer("azureml://test"), fl::Exception);
}

TEST(ModelRegistryClientTest, HandlesOptionalDescription) {
  ModelRegistryClient client("eastus", fl::test::NullLog(),
                             std::make_unique<RegionFallback>(fl::test::NullLog(), false),
                             [](const std::string&) {
                               return MakeRegistryResponse(R"({"blobSasUri": "https://example.com/blob?sig=x"})");
                             });
  auto container = client.ResolveModelContainer("azureml://test");
  EXPECT_EQ(container.blob_sas_uri, "https://example.com/blob?sig=x");
  EXPECT_TRUE(container.description.empty());
}

TEST(ModelRegistryClientTest, UrlEncodesAssetId) {
  std::string captured_url;
  ModelRegistryClient client("eastus", fl::test::NullLog(),
                             std::make_unique<RegionFallback>(fl::test::NullLog(), false),
                             [&captured_url](const std::string& url) {
                               captured_url = url;
                               return MakeRegistryResponse(R"({"blobSasUri": "https://example.com/blob"})");
                             });
  client.ResolveModelContainer("azureml://registries/test models/v1");
  // Spaces should be encoded as %20
  EXPECT_TRUE(captured_url.find("%20") != std::string::npos);
  EXPECT_TRUE(captured_url.find(" ") == std::string::npos);
}

TEST(ModelRegistryClientTest, Region_DefaultIsCentralUs) {
  std::string captured_url;
  ModelRegistryClient client("centralus", fl::test::NullLog(),
                             std::make_unique<RegionFallback>(fl::test::NullLog(), false),
                             [&captured_url](const std::string& url) {
                               captured_url = url;
                               return MakeRegistryResponse(R"({"blobSasUri": "https://example.com/blob"})");
                             });
  client.ResolveModelContainer("azureml://test");
  EXPECT_TRUE(captured_url.find("centralus.api.azureml.ms") != std::string::npos)
      << "Expected URL to target centralus region by default. Got: " << captured_url;
}

TEST(ModelRegistryClientTest, Region_PerCallOverridesDefault) {
  std::string captured_url;
  ModelRegistryClient client("centralus", fl::test::NullLog(),
                             std::make_unique<RegionFallback>(fl::test::NullLog(), false),
                             [&captured_url](const std::string& url) {
                               captured_url = url;
                               return MakeRegistryResponse(R"({"blobSasUri": "https://example.com/blob"})");
                             });
  // A non-empty per-call region selects that regional endpoint instead of the default.
  client.ResolveModelContainer("azureml://test", "westus2");
  EXPECT_TRUE(captured_url.find("westus2.api.azureml.ms") != std::string::npos)
      << "Expected per-call region to target westus2. Got: " << captured_url;
  EXPECT_TRUE(captured_url.find("centralus.api.azureml.ms") == std::string::npos)
      << "Expected per-call region to override the centralus default. Got: " << captured_url;
}

TEST(ModelRegistryClientTest, Region_EmptyPerCallUsesDefault) {
  std::string captured_url;
  ModelRegistryClient client("centralus", fl::test::NullLog(),
                             std::make_unique<RegionFallback>(fl::test::NullLog(), false),
                             [&captured_url](const std::string& url) {
                               captured_url = url;
                               return MakeRegistryResponse(R"({"blobSasUri": "https://example.com/blob"})");
                             });
  client.ResolveModelContainer("azureml://test", "");
  EXPECT_TRUE(captured_url.find("centralus.api.azureml.ms") != std::string::npos)
      << "Expected empty per-call region to fall back to the default. Got: " << captured_url;
}

TEST(ModelRegistryClientTest, Fallback_RetriesNextRegionOnRegionHealthFailure) {
  auto fallback =
      std::make_unique<RegionFallback>(fl::test::NullLog(), true, [](std::size_t) { return std::size_t{0}; });
  auto* fallback_observer = fallback.get();

  std::vector<std::string> attempted_urls;
  ModelRegistryClient client("eastus", fl::test::NullLog(), std::move(fallback), [&](const std::string& url) {
    attempted_urls.push_back(url);
    http::HttpResponse resp;
    if (attempted_urls.size() == 1) {
      resp.status = 503;  // first region (eastus) unhealthy
      return resp;
    }
    resp.status = 200;  // proximal region recovers
    resp.body = R"({"blobSasUri": "https://example.com/blob"})";
    return resp;
  });

  auto container = client.ResolveModelContainer("azureml://test", "eastus");
  EXPECT_EQ(container.blob_sas_uri, "https://example.com/blob");
  ASSERT_EQ(attempted_urls.size(), 2u);
  EXPECT_TRUE(attempted_urls[0].find("eastus.api.azureml.ms") != std::string::npos);
  EXPECT_TRUE(attempted_urls[1].find("eastus2.api.azureml.ms") != std::string::npos)
      << "Expected fallback to the first proximal region. Got: " << attempted_urls[1];

  // The healthy region becomes sticky for subsequent registry calls.
  auto sticky = fallback_observer->StickyRegion();
  ASSERT_TRUE(sticky.has_value());
  EXPECT_EQ(*sticky, "eastus2");
}

TEST(ModelRegistryClientTest, Fallback_PermanentErrorThrowsWithoutRetry) {
  auto fallback =
      std::make_unique<RegionFallback>(fl::test::NullLog(), true, [](std::size_t) { return std::size_t{0}; });

  int calls = 0;
  ModelRegistryClient client("eastus", fl::test::NullLog(), std::move(fallback), [&](const std::string&) {
    ++calls;
    http::HttpResponse resp;
    resp.status = 404;  // permanent — must not trigger cross-region retries
    return resp;
  });

  EXPECT_THROW(client.ResolveModelContainer("azureml://test", "eastus"), fl::Exception);
  EXPECT_EQ(calls, 1);
}

TEST(ModelRegistryClientTest, Fallback_PerCallRegionOverridesStickyRegion) {
  auto fallback =
      std::make_unique<RegionFallback>(fl::test::NullLog(), true, [](std::size_t) { return std::size_t{0}; });
  auto* fallback_observer = fallback.get();

  std::vector<std::string> attempted_urls;
  ModelRegistryClient client("eastus", fl::test::NullLog(), std::move(fallback), [&](const std::string& url) {
    attempted_urls.push_back(url);
    http::HttpResponse resp;
    if (attempted_urls.size() == 1) {
      resp.status = 503;
      return resp;
    }

    resp.status = 200;
    resp.body = R"({"blobSasUri": "https://example.com/blob"})";
    return resp;
  });

  client.ResolveModelContainer("azureml://first", "eastus");
  auto sticky = fallback_observer->StickyRegion();
  ASSERT_TRUE(sticky.has_value());
  EXPECT_EQ(*sticky, "eastus2");

  attempted_urls.clear();
  client.ResolveModelContainer("azureml://second", "westeurope");

  ASSERT_FALSE(attempted_urls.empty());
  EXPECT_TRUE(attempted_urls.front().find("westeurope.api.azureml.ms") != std::string::npos)
      << "Expected explicit per-call region to start at westeurope despite sticky region. Got: "
      << attempted_urls.front();
}

// ========================================================================
// Blob download orchestration tests
// ========================================================================

TEST(BlobDownloadTest, DownloadsAllBlobs) {
  auto tmpdir = TempPath::CreateTempDir();
  MockBlobDownloader mock;
  mock.blobs_to_return = {
      {"model/weights.safetensors", 1000},
      {"model/config.json", 100},
  };

  BlobDownloadOptions opts;
  opts.path_prefix = "model";
  DownloadBlobsToDirectoryForTest(mock, "https://test.blob/container?sig=x", tmpdir.string(), opts);

  EXPECT_EQ(mock.downloaded_blobs.size(), 2u);
}

TEST(BlobDownloadTest, FiltersByPathPrefix) {
  auto tmpdir = TempPath::CreateTempDir();
  MockBlobDownloader mock;
  mock.blobs_to_return = {
      {"variant-a/weights.safetensors", 1000},
      {"variant-b/other.bin", 500},
      {"unrelated/file.txt", 200},
  };

  BlobDownloadOptions opts;
  opts.path_prefix = "variant-a";
  DownloadBlobsToDirectoryForTest(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);

  ASSERT_EQ(mock.downloaded_blobs.size(), 1u);
  EXPECT_EQ(mock.downloaded_blobs[0], "variant-a/weights.safetensors");
}

TEST(BlobDownloadTest, FiltersOutInferenceModelJson) {
  auto tmpdir = TempPath::CreateTempDir();
  MockBlobDownloader mock;
  mock.blobs_to_return = {
      {"weights.safetensors", 1000},
      {"inference_model.json", 50},
      {"config.json", 100},
  };

  BlobDownloadOptions opts;
  DownloadBlobsToDirectoryForTest(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);

  ASSERT_EQ(mock.downloaded_blobs.size(), 2u);
  for (const auto& name : mock.downloaded_blobs) {
    EXPECT_NE(name, "inference_model.json") << "Should filter out inference_model.json";
  }
}

TEST(BlobDownloadTest, ReportsProgress) {
  auto tmpdir = TempPath::CreateTempDir();
  MockBlobDownloader mock;
  mock.blobs_to_return = {
      {"file1.bin", 500},
      {"file2.bin", 500},
  };

  std::vector<float> progress_values;
  BlobDownloadOptions opts;
  opts.progress = [&progress_values](float percent) {
    progress_values.push_back(percent);
    return 0;
  };

  DownloadBlobsToDirectoryForTest(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);

  // Should get progress reports, ending at 100
  ASSERT_FALSE(progress_values.empty());
  EXPECT_FLOAT_EQ(progress_values.back(), 100.0f);

  // All progress values should be in [0, 100]
  for (float v : progress_values) {
    EXPECT_GE(v, 0.0f);
    EXPECT_LE(v, 100.0f);
  }
}

TEST(BlobDownloadTest, HandlesEmptyBlobList) {
  auto tmpdir = TempPath::CreateTempDir();
  MockBlobDownloader mock;
  // No blobs

  BlobDownloadOptions opts;
  DownloadBlobsToDirectoryForTest(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);

  EXPECT_TRUE(mock.downloaded_blobs.empty());
}

// ========================================================================
// Skip-existing (Increment 1: resumable downloads)
// ========================================================================

TEST(BlobDownloadTest, SkipsExistingFilesWithMatchingPublishedChecksum) {
  auto tmpdir = TempPath::CreateTempDir();
  // Pre-create one of the blobs at the expected size on disk.
  std::ofstream(tmpdir.path() / "weights.safetensors") << std::string(1000, 'X');

  MockBlobDownloader mock;
  mock.blobs_to_return = {
      {"weights.safetensors", 1000, Md5(std::string(1000, 'X'))},
      {"config.json", 100},
  };

  BlobDownloadOptions opts;
  DownloadBlobsToDirectoryForTest(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);

  // Only the missing blob should be downloaded.
  ASSERT_EQ(mock.downloaded_blobs.size(), 1u);
  EXPECT_EQ(mock.downloaded_blobs[0], "config.json");
}

TEST(BlobDownloadTest, RedownloadsFilesWithWrongSize) {
  auto tmpdir = TempPath::CreateTempDir();
  // Existing file is truncated relative to the expected blob size.
  std::ofstream(tmpdir.path() / "weights.safetensors") << std::string(500, 'X');

  MockBlobDownloader mock;
  mock.blobs_to_return = {
      {"weights.safetensors", 1000},
  };

  BlobDownloadOptions opts;
  DownloadBlobsToDirectoryForTest(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);

  // Wrong-size files should be redownloaded (the mock overwrites them).
  ASSERT_EQ(mock.downloaded_blobs.size(), 1u);
  EXPECT_EQ(mock.downloaded_blobs[0], "weights.safetensors");
}

TEST(BlobDownloadTest, ReportsSkippedBytesInInitialProgress) {
  auto tmpdir = TempPath::CreateTempDir();
  // 500 of 2000 bytes already on disk → initial progress should be 25%.
  std::ofstream(tmpdir.path() / "already.bin") << std::string(500, 'X');

  MockBlobDownloader mock;
  mock.blobs_to_return = {
      {"already.bin", 500, Md5(std::string(500, 'X'))},
      {"missing.bin", 1500},
  };

  std::vector<float> progress_values;
  BlobDownloadOptions opts;
  opts.progress = [&](float pct) {
    progress_values.push_back(pct);
    return 0;
  };

  DownloadBlobsToDirectoryForTest(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);

  ASSERT_FALSE(progress_values.empty());
  // First emitted progress reflects the already-on-disk bytes (500/2000 = 25%).
  EXPECT_NEAR(progress_values.front(), 100.0f * 500.0f / 2000.0f, 0.5f);
  // Final progress must hit 100%.
  EXPECT_FLOAT_EQ(progress_values.back(), 100.0f);
}

TEST(BlobDownloadTest, EmitsHundredPercentWhenEverythingIsCached) {
  auto tmpdir = TempPath::CreateTempDir();
  std::ofstream(tmpdir.path() / "a.bin") << std::string(100, 'A');
  std::ofstream(tmpdir.path() / "b.bin") << std::string(200, 'B');

  MockBlobDownloader mock;
  mock.blobs_to_return = {
      {"a.bin", 100, Md5(std::string(100, 'A'))},
      {"b.bin", 200, Md5(std::string(200, 'B'))},
  };

  std::vector<float> progress_values;
  BlobDownloadOptions opts;
  opts.progress = [&](float pct) {
    progress_values.push_back(pct);
    return 0;
  };

  DownloadBlobsToDirectoryForTest(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);

  EXPECT_TRUE(mock.downloaded_blobs.empty());
  ASSERT_FALSE(progress_values.empty());
  EXPECT_FLOAT_EQ(progress_values.front(), 100.0f);
}

TEST(BlobDownloadTest, RedownloadsFullSizeFilesWithoutProofOfIntegrity) {
  auto tmpdir = TempPath::CreateTempDir();
  std::ofstream(tmpdir.path() / "weights.bin", std::ios::binary) << std::string(1000, '\0');

  MockBlobDownloader mock;
  mock.blobs_to_return = {{"weights.bin", 1000}};
  DownloadBlobsToDirectoryForTest(mock, "", tmpdir.string(), {});

  EXPECT_EQ(mock.downloaded_blobs, std::vector<std::string>{"weights.bin"});
}

TEST(BlobDownloadTest, RedownloadsSameSizeFilesWithIncorrectChecksum) {
  auto tmpdir = TempPath::CreateTempDir();
  std::ofstream(tmpdir.path() / "weights.bin", std::ios::binary) << std::string(1000, '\0');

  MockBlobDownloader mock;
  mock.blobs_to_return = {{"weights.bin", 1000, Md5(std::string(1000, 'X'))}};
  mock.download_content = std::string(1000, 'X');
  DownloadBlobsToDirectoryForTest(mock, "", tmpdir.string(), {});

  EXPECT_EQ(mock.downloaded_blobs, std::vector<std::string>{"weights.bin"});
}

TEST(BlobDownloadTest, RejectsDownloadedFileWithIncorrectPublishedChecksum) {
  auto tmpdir = TempPath::CreateTempDir();
  MockBlobDownloader mock;
  mock.blobs_to_return = {{"weights.bin", 1000, Md5(std::string(1000, 'X'))}};
  mock.download_content = std::string(1000, '\0');

  EXPECT_THROW(DownloadBlobsToDirectoryForTest(mock, "", tmpdir.string(), {}), fl::Exception);
}

// ========================================================================
// Path-traversal hardening (security)
// ========================================================================

TEST(IsPathWithinDirectoryTest, AcceptsChildPath) {
  fs::path root = fs::temp_directory_path() / "fl_root_a";
  EXPECT_TRUE(IsPathWithinDirectory(root / "child" / "leaf.bin", root));
}

TEST(IsPathWithinDirectoryTest, AcceptsRootItself) {
  fs::path root = fs::temp_directory_path() / "fl_root_b";
  EXPECT_TRUE(IsPathWithinDirectory(root, root));
}

TEST(IsPathWithinDirectoryTest, RejectsParentEscape) {
  fs::path root = fs::temp_directory_path() / "fl_root_c";
  EXPECT_FALSE(IsPathWithinDirectory(root / ".." / "evil.bin", root));
}

TEST(IsPathWithinDirectoryTest, RejectsDeepParentEscape) {
  fs::path root = fs::temp_directory_path() / "fl_root_d" / "sub";
  EXPECT_FALSE(IsPathWithinDirectory(root / ".." / ".." / ".." / "evil.bin", root));
}

TEST(IsPathWithinDirectoryTest, RejectsSiblingPrefixCollision) {
  // Guard against naive string prefix matching: "/foo/bar2" must not be
  // considered inside "/foo/bar".
  fs::path root = fs::temp_directory_path() / "fl_root_bar";
  fs::path sibling = fs::temp_directory_path() / "fl_root_bar2" / "leaf.bin";
  EXPECT_FALSE(IsPathWithinDirectory(sibling, root));
}

TEST(BlobDownloadTest, RejectsPathTraversalBlobName) {
  auto tmpdir = TempPath::CreateTempDir();
  MockBlobDownloader mock;
  mock.blobs_to_return = {
      {"../evil.bin", 4},
  };

  BlobDownloadOptions opts;
  EXPECT_THROW(DownloadBlobsToDirectoryForTest(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts),
               fl::Exception);
  EXPECT_TRUE(mock.downloaded_blobs.empty());
}

TEST(BlobDownloadTest, RejectsBackslashPathTraversalBlobName) {
  auto tmpdir = TempPath::CreateTempDir();
  MockBlobDownloader mock;
  mock.blobs_to_return = {
      {"..\\evil.bin", 4},
  };

  BlobDownloadOptions opts;
  EXPECT_THROW(DownloadBlobsToDirectoryForTest(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts),
               fl::Exception);
  EXPECT_TRUE(mock.downloaded_blobs.empty());
}

TEST(BlobDownloadTest, RejectsNestedPathTraversalBlobName) {
  auto tmpdir = TempPath::CreateTempDir();
  MockBlobDownloader mock;
  mock.blobs_to_return = {
      {"good/../../evil.bin", 4},
  };

  BlobDownloadOptions opts;
  EXPECT_THROW(DownloadBlobsToDirectoryForTest(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts),
               fl::Exception);
  EXPECT_TRUE(mock.downloaded_blobs.empty());
}

TEST(BlobDownloadTest, CancellationStopsRemainingBlobs) {
  auto tmpdir = TempPath::CreateTempDir();
  CancellingMockDownloader mock;
  mock.blobs_to_return = {
      {"blob1.bin", 100},
      {"blob2.bin", 100},
      {"blob3.bin", 100},
  };

  BlobDownloadOptions opts;
  opts.progress = [](float) {
    return 1;  // Cancel immediately on first progress report
  };

  try {
    DownloadBlobsToDirectoryForTest(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);
    FAIL() << "Expected fl::Exception to be thrown";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  }

  EXPECT_EQ(mock.download_count, 0)
      << "No blobs should have been downloaded — cancellation fired at 0% before any blob started";
}

TEST(BlobDownloadTest, FailedTransferPreservesTransferDuration) {
  auto tmpdir = TempPath::CreateTempDir();
  FailingTransferDownloader downloader;
  BlobDownloadStats stats;

  EXPECT_THROW(
      DownloadBlobsToDirectory(downloader, "https://test.blob/c?sig=x", tmpdir.string(), BlobDownloadOptions{}, stats,
                               fl::test::NullLog()),
      std::runtime_error);

  EXPECT_GE(stats.download_ms, 20);
}

TEST(BlobDownloadTest, CancelledFlagAbortsInFlightDownload) {
  auto tmpdir = TempPath::CreateTempDir();
  CancelCheckingMockDownloader mock;
  mock.blobs_to_return = {
      {"blob1.bin", 100},
      {"blob2.bin", 100},
  };

  // Allow the initial 0% progress through, then cancel on the first chunk progress.
  // This ensures DownloadBlob is actually called so the mock can verify the cancelled flag.
  int call_count = 0;
  BlobDownloadOptions opts;
  opts.progress = [&call_count](float) {
    return (++call_count > 1) ? 1 : 0;
  };

  try {
    DownloadBlobsToDirectoryForTest(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);
    FAIL() << "Expected fl::Exception to be thrown";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  }

  // The mock caught the callback exception and checked the cancelled flag —
  // only one blob should have been attempted.
  EXPECT_EQ(mock.download_count, 1);
}

// ========================================================================
// Inference model writer tests
// ========================================================================

TEST(InferenceModelWriterTest, WritesJsonWithPromptTemplate) {
  auto tmpdir = TempPath::CreateTempDir();
  fl::KeyValuePairs templates;
  templates.Add("system", "You are a helpful assistant.");
  templates.Add("user", "{input}");
  WriteInferenceModelJson(tmpdir.string(), "test-model", templates);

  auto content = ReadFile(tmpdir.path() / "inference_model.json");
  auto j = nlohmann::json::parse(content);

  EXPECT_EQ(j["Name"], "test-model");
  EXPECT_EQ(j["PromptTemplate"]["system"], "You are a helpful assistant.");
  EXPECT_EQ(j["PromptTemplate"]["user"], "{input}");
}

TEST(InferenceModelWriterTest, WritesNullPromptTemplateWhenEmpty) {
  auto tmpdir = TempPath::CreateTempDir();
  fl::KeyValuePairs templates;
  WriteInferenceModelJson(tmpdir.string(), "test-model", templates);

  auto content = ReadFile(tmpdir.path() / "inference_model.json");
  auto j = nlohmann::json::parse(content);

  EXPECT_EQ(j["Name"], "test-model");
  EXPECT_TRUE(j["PromptTemplate"].is_null());
}

// ========================================================================
// Variant fixup tests
// ========================================================================

TEST(VariantFixupTest, CopiesInferenceModelToSubdirs) {
  auto tmpdir = TempPath::CreateTempDir();
  const auto& root = tmpdir.path();

  // Create inference_model.json at root
  {
    std::ofstream f(root / "inference_model.json");
    f << R"({"Name": "test"})";
  }

  // Create subdirectories (simulating variant blobs)
  fs::create_directories(root / "variant-a");
  fs::create_directories(root / "variant-b");

  FixVariantInferenceModelJson(root.string());

  // inference_model.json should be in both subdirs
  EXPECT_TRUE(fs::exists(root / "variant-a" / "inference_model.json"));
  EXPECT_TRUE(fs::exists(root / "variant-b" / "inference_model.json"));
  // Root copy should be deleted
  EXPECT_FALSE(fs::exists(root / "inference_model.json"));
}

TEST(VariantFixupTest, DoesNotOverwriteExisting) {
  auto tmpdir = TempPath::CreateTempDir();
  const auto& root = tmpdir.path();

  {
    std::ofstream f(root / "inference_model.json");
    f << R"({"Name": "root"})";
  }

  fs::create_directories(root / "variant-a");
  {
    std::ofstream f(root / "variant-a" / "inference_model.json");
    f << R"({"Name": "existing"})";
  }

  FixVariantInferenceModelJson(root.string());

  // Existing file should not be overwritten
  auto content = ReadFile(root / "variant-a" / "inference_model.json");
  auto j = nlohmann::json::parse(content);
  EXPECT_EQ(j["Name"], "existing");
}

TEST(VariantFixupTest, NoOpWhenNoRootFile) {
  auto tmpdir = TempPath::CreateTempDir();
  fs::create_directories(tmpdir.path() / "sub");

  // Should not throw
  FixVariantInferenceModelJson(tmpdir.string());
  EXPECT_FALSE(fs::exists(tmpdir.path() / "sub" / "inference_model.json"));
}

TEST(VariantFixupTest, PreservesRootFileWhenNoSubdirs) {
  // Single-variant downloads put every blob (and inference_model.json) at the root
  // with no variant subdirectory. The fixup must not delete the root file in that
  // case, otherwise IsModelCached would report false on the next check.
  auto tmpdir = TempPath::CreateTempDir();
  const auto& root = tmpdir.path();

  {
    std::ofstream f(root / "inference_model.json");
    f << R"({"Name": "root-only"})";
  }

  // Add a sibling blob file (not a directory) to make sure the iterator's
  // is_directory() filter doesn't accidentally count it as a variant.
  {
    std::ofstream f(root / "weights.safetensors");
    f << "blob";
  }

  FixVariantInferenceModelJson(root.string());

  EXPECT_TRUE(fs::exists(root / "inference_model.json"));
  auto content = ReadFile(root / "inference_model.json");
  auto j = nlohmann::json::parse(content);
  EXPECT_EQ(j["Name"], "root-only");
}

// ========================================================================
// DownloadManager tests
// ========================================================================

class ModelCacheRemovalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    services_.download_manager.SetCacheDirectory(cache_.string());
    info_.model_id = "test-model:1";
    info_.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "TestPublisher";
    root_ = fs::path(cache_.string()) / "TestPublisher" / "test-model-1";
  }

  Model MakeModel(const std::string& path = "") {
    return Model::FromModelInfo(info_, path, services_.download_manager, services_.model_load_manager);
  }

  TempPath cache_ = TempPath::CreateTempDir();
  fl::test::FakeServiceBindings services_;
  ModelInfo info_;
  fs::path root_;
};

TEST_F(ModelCacheRemovalTest, RemovesIncompleteDownloadWithoutCachedPath) {
  fs::create_directories(root_ / "v3");
  std::ofstream(root_ / "download.tmp") << "";
  std::ofstream(root_ / ".download.progress") << "42";
  std::ofstream(root_ / "v3" / "weights.onnx") << "partial payload";
  std::ofstream(root_ / "v3" / "weights.onnx.dlstate") << "partial state";
  auto model = MakeModel();
  ASSERT_FALSE(model.IsCached());

  ASSERT_NO_THROW(model.RemoveFromCache());

  EXPECT_FALSE(model.IsCached());
  EXPECT_TRUE(model.GetPath().empty());
  EXPECT_FALSE(services_.download_manager.IsModelCached(info_));
  EXPECT_TRUE(fs::is_empty(root_));
}

TEST_F(ModelCacheRemovalTest, RemovesWholeRootForRevisionPathAndPreservesOtherModels) {
  fs::create_directories(root_ / "v3");
  fs::create_directories(root_ / "v2");
  std::ofstream(root_ / "v3" / "genai_config.json") << "{}";
  std::ofstream(root_ / "v3" / "inference_model.json") << "{}";
  std::ofstream(root_ / ".download.progress") << "100";
  auto sibling = root_.parent_path() / "other-model-1";
  fs::create_directories(sibling);
  std::ofstream(sibling / "weights.onnx") << "other model";
  auto catalog_metadata = fs::path(cache_.string()) / "foundry.modelinfo.json";
  std::ofstream(catalog_metadata) << "shared metadata";
  auto model = MakeModel((root_ / "v3").string());
  ASSERT_TRUE(model.IsCached());
  ASSERT_TRUE(services_.download_manager.IsModelCached(info_));

  ASSERT_NO_THROW(model.RemoveFromCache());

  EXPECT_FALSE(model.IsCached());
  EXPECT_TRUE(model.GetPath().empty());
  EXPECT_FALSE(services_.download_manager.IsModelCached(info_));
  EXPECT_TRUE(fs::is_empty(root_));
  EXPECT_EQ(ReadFile(sibling / "weights.onnx"), "other model");
  EXPECT_EQ(ReadFile(catalog_metadata), "shared metadata");
  EXPECT_TRUE(ScanLocalModels(cache_.string(), fl::test::NullLog()).empty());
}

TEST_F(ModelCacheRemovalTest, RejectsConflictingMetadataAnywhereInComputedRoot) {
  for (const auto& relative_path : {fs::path{}, fs::path("v3"), fs::path("v3") / "nested"}) {
    for (const bool cached_handle : {false, true}) {
      SCOPED_TRACE(relative_path);
      SCOPED_TRACE(cached_handle);
      const auto other_path = relative_path.empty() ? root_ : root_ / relative_path;
      fs::create_directories(other_path);
      WriteInferenceModelJson(other_path.string(), "other-model:1", {});
      std::ofstream(other_path / "genai_config.json") << "{}";
      std::ofstream(other_path / "weights.onnx") << "preserve";
      std::ofstream(root_ / ".download.progress") << "42";
      const auto scanned = ScanLocalModels(cache_.string(), fl::test::NullLog());
      ASSERT_EQ(scanned.at("other-model:1"), other_path.string());
      ASSERT_EQ(scanned.count(info_.model_id), 0u);
      auto model = MakeModel(cached_handle ? other_path.string() : "");

      EXPECT_THROW(model.RemoveFromCache(), fl::Exception);

      EXPECT_EQ(model.IsCached(), cached_handle);
      EXPECT_EQ(ReadFile(other_path / "weights.onnx"), "preserve");
      EXPECT_EQ(ReadFile(root_ / ".download.progress"), "42");
      EXPECT_EQ(ScanLocalModels(cache_.string(), fl::test::NullLog()), scanned);
      fs::remove_all(root_);
    }
  }
}

TEST_F(ModelCacheRemovalTest, ChecksComputedRootOwnershipAfterTakingDownloadLock) {
  fs::create_directories(root_ / "v3");
  WriteInferenceModelJson((root_ / "v3").string(), info_.model_id, {});
  std::ofstream(root_ / "v3" / "weights.onnx") << "preserve";
  CacheOperationLogger logger;
  bool ownership_changed = false;
  const auto lock_acquired = "CrossProcessFileLock acquired: " + (root_ / ".download.lock").string();
  logger.on_log = [&](std::string_view message) {
    if (message == lock_acquired) {
      ownership_changed = true;
      WriteInferenceModelJson((root_ / "v3").string(), "other-model:1", {});
    }
  };
  DownloadManager manager(cache_.string(), "eastus", 1, logger, fl::test::TestTelemetrySink());
  auto model = Model::FromModelInfo(info_, "", manager, services_.model_load_manager);

  EXPECT_THROW(model.RemoveFromCache(), fl::Exception);

  EXPECT_TRUE(ownership_changed);
  EXPECT_EQ(ReadFile(root_ / "v3" / "weights.onnx"), "preserve");
}

TEST_F(ModelCacheRemovalTest, RejectsLoadedSiblingRevisionForCachedAndUncachedHandles) {
  for (const bool nonstandard : {false, true}) {
    for (const bool cached_handle : {false, true}) {
      if (nonstandard && !cached_handle) {
        continue;  // An uncached handle has no scanner-discovered nonstandard path.
      }

      SCOPED_TRACE(nonstandard);
      SCOPED_TRACE(cached_handle);
      const auto deletion_root = nonstandard ? fs::path(cache_.string()) / "imported-model" : root_;
      const auto loaded_path = deletion_root / "v2";
      fs::create_directories(deletion_root);
      fs::copy(fl::test::GetTestDataPath("tiny-random-gpt2-fp32-1"), loaded_path, fs::copy_options::recursive);
      fs::create_directories(deletion_root / "v3");
      std::ofstream(deletion_root / "v3" / "weights.onnx") << "preserve";
      std::ofstream(deletion_root / ".download.progress") << "42";
      WriteInferenceModelJson(loaded_path.string(), info_.model_id, {});
      const auto loaded = services_.model_load_manager.LoadModel(loaded_path.string(), info_.model_id);
      ASSERT_EQ(loaded.status, ModelLoadManager::LoadStatus::kSuccess);
      auto model = MakeModel(cached_handle ? (deletion_root / "v3").string() : "");
      ASSERT_FALSE(model.IsLoaded());

      EXPECT_THROW(model.RemoveFromCache(), fl::Exception);

      EXPECT_EQ(model.IsCached(), cached_handle);
      EXPECT_TRUE(fs::exists(loaded_path / "genai_config.json"));
      EXPECT_EQ(ReadFile(deletion_root / "v3" / "weights.onnx"), "preserve");
      EXPECT_EQ(ReadFile(deletion_root / ".download.progress"), "42");
      EXPECT_EQ(services_.model_load_manager.GetLoadedModel(info_.model_id), loaded.model);
      ASSERT_TRUE(services_.model_load_manager.UnloadModel(info_.model_id));
      EXPECT_NO_THROW(model.RemoveFromCache());
      EXPECT_TRUE(fs::is_empty(deletion_root));
    }
  }
}

TEST_F(ModelCacheRemovalTest, RejectsLoadedFilesWithDifferentIdAndCorruptMetadata) {
  const auto loaded_path = root_ / "v2";
  fs::create_directories(root_);
  fs::copy(fl::test::GetTestDataPath("tiny-random-gpt2-fp32-1"), loaded_path, fs::copy_options::recursive);
  std::ofstream(loaded_path / "inference_model.json") << "corrupt metadata";
  const auto loaded = services_.model_load_manager.LoadModel(loaded_path.string(), "other-model:1");
  ASSERT_EQ(loaded.status, ModelLoadManager::LoadStatus::kSuccess);
  auto model = MakeModel();

  EXPECT_THROW(model.RemoveFromCache(), fl::Exception);

  EXPECT_TRUE(fs::exists(loaded_path / "genai_config.json"));
  EXPECT_EQ(ReadFile(loaded_path / "inference_model.json"), "corrupt metadata");
  EXPECT_EQ(services_.model_load_manager.GetLoadedModel("other-model:1"), loaded.model);
  ASSERT_TRUE(services_.model_load_manager.UnloadModel("other-model:1"));
  EXPECT_NO_THROW(model.RemoveFromCache());
}

TEST_F(ModelCacheRemovalTest, AllowsRemovalWhenSameIdIsLoadedOutsideDeletionRoot) {
  const auto loaded_path = fl::test::GetTestDataPath("tiny-random-gpt2-fp32-1");
  const auto loaded = services_.model_load_manager.LoadModel(loaded_path.string(), info_.model_id);
  ASSERT_EQ(loaded.status, ModelLoadManager::LoadStatus::kSuccess);
  fs::create_directories(root_);
  std::ofstream(root_ / "weights.onnx") << "remove";
  auto model = MakeModel(root_.string());
  ASSERT_FALSE(model.IsLoaded());

  EXPECT_NO_THROW(model.RemoveFromCache());

  EXPECT_FALSE(model.IsCached());
  EXPECT_TRUE(fs::is_empty(root_));
  EXPECT_EQ(services_.model_load_manager.GetLoadedModel(info_.model_id), loaded.model);
  EXPECT_TRUE(fs::exists(loaded_path / "genai_config.json"));
}

TEST_F(ModelCacheRemovalTest, RemovesCorruptDownloadAndCanDownloadAgain) {
  auto registry = std::make_unique<ModelRegistryClient>(
      "eastus", fl::test::NullLog(), std::make_unique<RegionFallback>(fl::test::NullLog(), false),
      [](const std::string&) {
        return MakeRegistryResponse(R"({"blobSasUri": "https://storage.test/container?sig=test"})");
      });
  auto downloader = std::make_unique<MockBlobDownloader>();
  downloader->blobs_to_return = {{"v3/genai_config.json", 100}, {"v3/weights.onnx", 1024}};
  auto* mock = downloader.get();
  DownloadManager manager(cache_.string(), "eastus", 1, fl::test::NullLog(), fl::test::TestTelemetrySink(),
                          false, std::move(registry), std::move(downloader));
  info_.uri = "azureml://registries/test/models/test-model/versions/1";
  fs::create_directories(root_ / "v3");
  std::ofstream(root_ / "v3" / "inference_model.json") << "invalid json";
  std::ofstream(root_ / "v3" / "weights.onnx") << "corrupt payload";
  auto model = Model::FromModelInfo(info_, (root_ / "v3").string(), manager, services_.model_load_manager);

  ASSERT_NO_THROW(model.RemoveFromCache());
  ASSERT_FALSE(model.IsCached());
  std::vector<float> progress;
  ASSERT_NO_THROW(model.Download([&](float percent) {
    if (progress.empty()) {
      EXPECT_THROW(model.RemoveFromCache(), fl::Exception);
    }
    progress.push_back(percent);
    return 0;
  }));

  EXPECT_TRUE(model.IsCached());
  EXPECT_TRUE(manager.IsModelCached(info_));
  ASSERT_FALSE(progress.empty());
  EXPECT_EQ(progress.front(), 0.0f);
  EXPECT_EQ(mock->downloaded_blobs.size(), 2u);
  EXPECT_EQ(ReadFile(root_ / "v3" / "weights.onnx"), "mock content for v3/weights.onnx");
}

TEST_F(ModelCacheRemovalTest, PreservesScannerDiscoveredNonstandardPath) {
  auto scanned_path = fs::path(cache_.string()) / "imported-model";
  fs::create_directories(scanned_path);
  std::ofstream(scanned_path / "weights.onnx") << "payload";
  fs::create_directories(root_);
  std::ofstream(root_ / "unrelated.txt") << "preserve";
  auto model = MakeModel(scanned_path.string());

  ASSERT_NO_THROW(model.RemoveFromCache());

  EXPECT_FALSE(model.IsCached());
  EXPECT_TRUE(fs::is_empty(scanned_path));
  EXPECT_EQ(ReadFile(root_ / "unrelated.txt"), "preserve");
}

TEST_F(ModelCacheRemovalTest, RejectsRemovalWhileDownloadLockIsHeld) {
  fs::create_directories(root_ / "v3");
  std::ofstream(root_ / "v3" / "weights.onnx") << "payload";
  auto model = MakeModel((root_ / "v3").string());
  auto lock = CrossProcessFileLock::TryAcquireForDirectory(root_, fl::test::NullLog());
  ASSERT_NE(lock, nullptr);

  EXPECT_THROW(model.RemoveFromCache(), fl::Exception);

  EXPECT_TRUE(model.IsCached());
  EXPECT_EQ(ReadFile(root_ / "v3" / "weights.onnx"), "payload");
}

TEST_F(ModelCacheRemovalTest, RemovesNonstandardRevisionRootWithDownloadState) {
  for (const auto* marker : {"download.tmp", ".download.progress"}) {
    SCOPED_TRACE(marker);
    const auto imported_root = fs::path(cache_.string()) / "imported-model";
    fs::create_directories(imported_root / "v3");
    fs::create_directories(imported_root / "v2");
    WriteInferenceModelJson((imported_root / "v3").string(), info_.model_id, {});
    std::ofstream(imported_root / "v3" / "genai_config.json") << "{}";
    std::ofstream(imported_root / "v3" / "weights.onnx") << "payload";
    std::ofstream(imported_root / "v3" / "weights.onnx.dlstate") << "resume state";
    std::ofstream(imported_root / "v2" / "inference_model.json") << "corrupt metadata";
    fs::create_directories(root_);
    std::ofstream(root_ / "unrelated.txt") << "preserve";
    const auto scanned = ScanLocalModels(cache_.string(), fl::test::NullLog());
    ASSERT_EQ(scanned.at(info_.model_id), (imported_root / "v3").string());
    auto model = MakeModel(scanned.at(info_.model_id));
    std::ofstream(imported_root / marker) << "42";

    ASSERT_NO_THROW(model.RemoveFromCache());

    EXPECT_TRUE(fs::is_empty(imported_root));
    EXPECT_FALSE(model.IsCached());
    EXPECT_TRUE(model.GetPath().empty());
    EXPECT_TRUE(ScanLocalModels(cache_.string(), fl::test::NullLog()).empty());
    EXPECT_EQ(ReadFile(root_ / "unrelated.txt"), "preserve");
  }
}

TEST_F(ModelCacheRemovalTest, RejectsNonstandardRevisionRemovalWhileParentLockIsHeld) {
  for (const bool ambiguous_parent : {false, true}) {
    SCOPED_TRACE(ambiguous_parent);
    const auto imported_root = fs::path(cache_.string()) / "imported-model";
    fs::create_directories(imported_root / "v3");
    std::ofstream(imported_root / ".download.progress") << "42";
    std::ofstream(imported_root / "v3" / "weights.onnx") << "preserve";
    if (ambiguous_parent) {
      std::ofstream(imported_root / "notes.txt") << "unrelated";
    }

    auto model = MakeModel((imported_root / "v3").string());
    auto lock = CrossProcessFileLock::TryAcquireForDirectory(imported_root, fl::test::NullLog());
    ASSERT_NE(lock, nullptr);

    EXPECT_THROW(model.RemoveFromCache(), fl::Exception);

    EXPECT_TRUE(model.IsCached());
    EXPECT_EQ(ReadFile(imported_root / "v3" / "weights.onnx"), "preserve");
    EXPECT_EQ(ReadFile(imported_root / ".download.progress"), "42");
  }
}

TEST_F(ModelCacheRemovalTest, HandlesRootMetadataInNonstandardRevisionCaches) {
  const struct {
    const char* marker;
    const char* metadata;
    bool remove_parent;
  } cases[] = {
      {"download.tmp", R"({"Name":"test-model:1"})", true},
      {".download.progress", R"({"Name":"test-model:1"})", true},
      {"", R"({"Name":"test-model:1"})", true},
      {"download.tmp", "invalid json", true},
      {".download.progress", "invalid json", true},
      {"", "invalid json", false},
      {".download.progress", R"({"Name":"other-model:1"})", false},
      {"", R"({"Name":"other-model:1"})", false},
      {".download.progress", R"({"Name":""})", true},
  };

  int case_index = 0;
  for (const auto& test_case : cases) {
    SCOPED_TRACE(case_index);
    const auto imported_root = fs::path(cache_.string()) / ("imported-model-" + std::to_string(case_index++));
    fs::create_directories(imported_root / "v3");
    fs::create_directories(imported_root / "v2");
    std::ofstream(imported_root / "inference_model.json") << test_case.metadata;
    std::ofstream(imported_root / "v2" / "weights.onnx") << "older revision";
    std::ofstream(imported_root / "v3" / "weights.onnx") << "current revision";
    if (test_case.marker[0] != '\0') {
      std::ofstream(imported_root / test_case.marker) << "42";
    }

    auto model = MakeModel((imported_root / "v3").string());

    ASSERT_NO_THROW(model.RemoveFromCache());

    EXPECT_FALSE(model.IsCached());
    EXPECT_TRUE(model.GetPath().empty());
    if (test_case.remove_parent) {
      EXPECT_TRUE(fs::is_empty(imported_root));
    } else {
      EXPECT_TRUE(fs::is_empty(imported_root / "v3"));
      EXPECT_EQ(ReadFile(imported_root / "inference_model.json"), test_case.metadata);
      EXPECT_EQ(ReadFile(imported_root / "v2" / "weights.onnx"), "older revision");
      if (test_case.marker[0] != '\0') {
        EXPECT_EQ(ReadFile(imported_root / test_case.marker), "42");
      }
    }
  }
}

TEST_F(ModelCacheRemovalTest, RechecksNonstandardOwnershipAfterTakingParentLock) {
  const auto imported_root = fs::path(cache_.string()) / "imported-model";
  fs::create_directories(imported_root / "v3");
  fs::create_directories(imported_root / "v2");
  std::ofstream(imported_root / ".download.progress") << "42";
  WriteInferenceModelJson((imported_root / "v2").string(), info_.model_id, {});
  CacheOperationLogger logger;
  const auto parent_acquired = "CrossProcessFileLock acquired: " + (imported_root / ".download.lock").string();
  const auto child_acquired = "CrossProcessFileLock acquired: " + (imported_root / "v3" / ".download.lock").string();
  bool ownership_changed = false;
  bool parent_held_during_fallback = false;
  logger.on_log = [&](std::string_view message) {
    if (message == parent_acquired) {
      ownership_changed = true;
      WriteInferenceModelJson((imported_root / "v2").string(), "other-model:1", {});
    } else if (message == child_acquired) {
      parent_held_during_fallback = true;
      EXPECT_EQ(CrossProcessFileLock::TryAcquireForDirectory(imported_root, fl::test::NullLog()), nullptr);
    }
  };

  DownloadManager manager(cache_.string(), "eastus", 1, logger, fl::test::TestTelemetrySink());
  auto model = Model::FromModelInfo(info_, (imported_root / "v3").string(), manager, services_.model_load_manager);

  ASSERT_NO_THROW(model.RemoveFromCache());

  EXPECT_TRUE(ownership_changed);
  EXPECT_TRUE(parent_held_during_fallback);
  EXPECT_TRUE(fs::is_empty(imported_root / "v3"));
  EXPECT_EQ(ReadFile(imported_root / ".download.progress"), "42");
  EXPECT_EQ(nlohmann::json::parse(ReadFile(imported_root / "v2" / "inference_model.json"))["Name"], "other-model:1");
}

TEST_F(ModelCacheRemovalTest, RejectsRemovalDuringCachedDownloadCallbacksAndReleasesGuardOnFailure) {
  fs::create_directories(root_);
  std::ofstream(root_ / "weights.onnx") << "preserve";
  auto model = MakeModel(root_.string());

  EXPECT_NO_THROW(model.Download([&](float percent) {
    EXPECT_EQ(percent, 100.0f);
    EXPECT_THROW(model.RemoveFromCache(), fl::Exception);
    return 0;
  }));

  EXPECT_TRUE(model.IsCached());
  EXPECT_EQ(ReadFile(root_ / "weights.onnx"), "preserve");
  EXPECT_THROW(model.Download([](float) -> int { throw std::runtime_error("callback failed"); }), std::runtime_error);
  EXPECT_NO_THROW(model.RemoveFromCache());
  EXPECT_TRUE(fs::is_empty(root_));
}

TEST_F(ModelCacheRemovalTest, RejectsRemovalBetweenDownloadLockReleaseAndCachePublication) {
  auto registry = std::make_unique<ModelRegistryClient>(
      "eastus", fl::test::NullLog(), std::make_unique<RegionFallback>(fl::test::NullLog(), false),
      [](const std::string&) {
        return MakeRegistryResponse(R"({"blobSasUri": "https://storage.test/container?sig=test"})");
      });
  auto downloader = std::make_unique<MockBlobDownloader>();
  downloader->blobs_to_return = {{"v3/genai_config.json", 100}, {"v3/weights.onnx", 1024}};
  CacheOperationLogger logger;
  DownloadManager manager(cache_.string(), "eastus", 1, logger, fl::test::TestTelemetrySink(),
                          false, std::move(registry), std::move(downloader));
  info_.uri = "azureml://registries/test/models/test-model/versions/1";
  auto model = Model::FromModelInfo(info_, "", manager, services_.model_load_manager);
  const auto released = "CrossProcessFileLock released: " + (root_ / ".download.lock").string();
  std::mutex mutex;
  std::condition_variable cv;
  bool download_paused = false;
  bool allow_publication = false;
  logger.on_log = [&](std::string_view message) {
    if (message == released) {
      std::unique_lock<std::mutex> lock(mutex);
      if (!download_paused) {
        download_paused = true;
        cv.notify_all();
        EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(10), [&] { return allow_publication; }));
      }
    }
  };

  std::exception_ptr download_failure;
  std::thread download([&] {
    try {
      model.Download();
    } catch (...) {
      download_failure = std::current_exception();
    }
  });

  bool observed_release;
  {
    std::unique_lock<std::mutex> lock(mutex);
    observed_release = cv.wait_for(lock, std::chrono::seconds(5), [&] { return download_paused; });
  }

  if (observed_release) {
    EXPECT_THROW(model.RemoveFromCache(), fl::Exception);
  }

  {
    std::lock_guard<std::mutex> lock(mutex);
    allow_publication = true;
  }

  cv.notify_all();
  download.join();
  ASSERT_TRUE(observed_release);
  if (download_failure) {
    std::rethrow_exception(download_failure);
  }

  EXPECT_TRUE(model.IsCached());
  EXPECT_EQ(model.GetPath(), (root_ / "v3").string());
  EXPECT_TRUE(fs::exists(root_ / "v3" / "weights.onnx"));
  EXPECT_NO_THROW(model.RemoveFromCache());
  logger.on_log = {};
}

TEST_F(ModelCacheRemovalTest, FailedDownloadDoesNotBlockLaterRemoval) {
  auto model = MakeModel();
  EXPECT_THROW(model.Download(), fl::Exception);
  fs::create_directories(root_);
  std::ofstream(root_ / "download.tmp") << "";

  EXPECT_NO_THROW(model.RemoveFromCache());
  EXPECT_TRUE(fs::is_empty(root_));
}

TEST_F(ModelCacheRemovalTest, DoesNotExpandNonstandardRemovalAcrossUnrelatedEntries) {
  for (const auto* unrelated : {"notes.txt", "other-model", "v2"}) {
    SCOPED_TRACE(unrelated);
    const auto parent = fs::path(cache_.string()) / unrelated;
    fs::create_directories(parent / "v3");
    std::ofstream(parent / ".download.progress") << "preserve";
    const auto other = parent / unrelated;
    if (std::string(unrelated) == "notes.txt") {
      std::ofstream(other) << "preserve";
    } else {
      fs::create_directories(other);
      WriteInferenceModelJson(other.string(), "other-model:1", {});
    }

    auto model = MakeModel((parent / "v3").string());

    ASSERT_NO_THROW(model.RemoveFromCache());

    EXPECT_TRUE(fs::is_empty(parent / "v3"));
    EXPECT_TRUE(fs::exists(other));
    EXPECT_EQ(ReadFile(parent / ".download.progress"), "preserve");
  }
}

TEST_F(ModelCacheRemovalTest, DoesNotExpandNonstandardRemovalWithoutDownloadStateOrIntoCacheRoot) {
  for (const bool use_cache_root : {false, true}) {
    SCOPED_TRACE(use_cache_root);
    const auto parent = use_cache_root ? fs::path(cache_.string()) : fs::path(cache_.string()) / "imported-model";
    fs::create_directories(parent / "v3");
    fs::create_directories(parent / "v2");
    std::ofstream(parent / "v2" / "weights.onnx") << "preserve";
    if (use_cache_root) {
      std::ofstream(parent / ".download.progress") << "preserve";
    }

    auto model = MakeModel((parent / "v3").string());

    ASSERT_NO_THROW(model.RemoveFromCache());

    EXPECT_TRUE(fs::is_empty(parent / "v3"));
    EXPECT_EQ(ReadFile(parent / "v2" / "weights.onnx"), "preserve");
  }
}

TEST_F(ModelCacheRemovalTest, RejectsUncachedModelWithoutDownloadState) {
  auto model = MakeModel();

  EXPECT_THROW(model.RemoveFromCache(), fl::Exception);
  EXPECT_FALSE(fs::exists(root_));
}

TEST_F(ModelCacheRemovalTest, RejectsExternalRegistrationAndRetiredModel) {
  fs::create_directories(root_);
  std::ofstream(root_ / "weights.onnx") << "preserve";
  auto registered = Model::FromLocalRegistration(info_, root_.string(), services_.download_manager,
                                                 services_.model_load_manager);
  auto retired = MakeModel(root_.string());
  retired.Deactivate();

  EXPECT_THROW(registered.RemoveFromCache(), fl::Exception);
  EXPECT_THROW(retired.RemoveFromCache(), fl::Exception);
  EXPECT_EQ(ReadFile(root_ / "weights.onnx"), "preserve");
}

TEST_F(ModelCacheRemovalTest, ClearsCachedStateWhenFilesWereAlreadyDeleted) {
  auto model = MakeModel((root_ / "v3").string());
  ASSERT_TRUE(model.IsCached());

  EXPECT_NO_THROW(model.RemoveFromCache());

  EXPECT_FALSE(model.IsCached());
  EXPECT_TRUE(model.GetPath().empty());
  EXPECT_FALSE(fs::exists(root_));
}

TEST_F(ModelCacheRemovalTest, RejectsCacheRootAndPathsOutsideCache) {
  auto outside = TempPath::CreateTempDir();
  std::ofstream(fs::path(outside.string()) / "weights.onnx") << "preserve";
  auto outside_model = MakeModel(outside.string());
  auto root_model = MakeModel(cache_.string());

  EXPECT_THROW(outside_model.RemoveFromCache(), fl::Exception);
  EXPECT_THROW(root_model.RemoveFromCache(), fl::Exception);
  EXPECT_TRUE(outside_model.IsCached());
  EXPECT_TRUE(root_model.IsCached());
  EXPECT_EQ(ReadFile(fs::path(outside.string()) / "weights.onnx"), "preserve");
}

TEST(DownloadManagerTest, FullDownloadFlow) {
  auto tmpdir = TempPath::CreateTempDir();
  RecordingDownloadTelemetry telemetry;

  auto registry = std::make_unique<ModelRegistryClient>(
      "eastus", fl::test::NullLog(), std::make_unique<RegionFallback>(fl::test::NullLog(), false),
      [](const std::string&) {
        return MakeRegistryResponse(
            R"({"blobSasUri": "https://storage.blob.core.windows.net/container?sig=test"})");
      });

  auto mock_downloader = std::make_unique<MockBlobDownloader>();
  mock_downloader->expected_sas_uri =
      "https://storage.blob.core.windows.net/container?sig=test";
  mock_downloader->blobs_to_return = {
      {"weights.safetensors", 1024},
      {"config.json", 100},
  };

  auto manager = std::make_unique<DownloadManager>(
      tmpdir.string(), "eastus", 64, fl::test::NullLog(), telemetry,
      /*disable_region_fallback=*/false, std::move(registry), std::move(mock_downloader));

  ModelInfo info;
  info.model_id = "test-model:1";
  info.name = "test-model";
  info.uri = "azureml://registries/test/models/test-model/versions/1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "TestPublisher";

  std::vector<float> progress_values;
  auto path = manager->DownloadModel(info, [&](float p) {
    progress_values.push_back(p);
    return 0;
  });

  // Verify the model path
  EXPECT_FALSE(path.empty());
  EXPECT_TRUE(fs::exists(path));

  // Verify inference_model.json was written (possibly moved by variant fixup)
  // The download signal file should be removed
  EXPECT_FALSE(fs::exists(fs::path(path) / "download.tmp"));

  // Verify progress was reported
  EXPECT_FALSE(progress_values.empty());
  ASSERT_EQ(telemetry.calls.size(), 1u);
  const auto& event = telemetry.calls[0];
  EXPECT_EQ(event.model_id, info.model_id);
  EXPECT_EQ(event.status, ActionStatus::kSuccess);
  EXPECT_EQ(event.file_count, 2);
  EXPECT_EQ(event.total_size_bytes, 1124);
  EXPECT_EQ(event.already_cached_bytes, 0);
  EXPECT_EQ(event.skipped_file_count, 0);
  EXPECT_EQ(event.max_concurrency, 64);
  EXPECT_EQ(event.download_wait_result, "Completed");
  EXPECT_FALSE(event.user_agent.empty());
  EXPECT_FALSE(event.correlation_id.empty());
  EXPECT_GE(event.lock_wait_ms, 0);
  EXPECT_GE(event.enumeration_ms, 0);
  EXPECT_GE(event.download_ms, 0);
}

TEST(DownloadManagerTest, CanceledDownloadRecordsCanceledEvent) {
  auto tmpdir = TempPath::CreateTempDir();
  RecordingDownloadTelemetry telemetry;
  DownloadManager manager(tmpdir.string(), "eastus", 4, fl::test::NullLog(), telemetry);
  ModelInfo info;
  info.model_id = "canceled-model:1";
  info.uri = "azureml://registries/test/models/canceled-model/versions/1";

  EXPECT_THROW(manager.DownloadModel(info, "test-client/1", [](float) { return 1; }), fl::Exception);

  ASSERT_EQ(telemetry.calls.size(), 1u);
  EXPECT_EQ(telemetry.calls[0].status, ActionStatus::kCanceled);
  EXPECT_EQ(telemetry.calls[0].model_id, info.model_id);
  EXPECT_EQ(telemetry.calls[0].user_agent, "test-client/1");
  EXPECT_EQ(telemetry.calls[0].download_wait_result, "Canceled");
  EXPECT_EQ(telemetry.calls[0].download_ms, 0);
}

// --- Region resolution: detected region drives the download endpoint ---

// Run one download and return the registry URL the manager hit.
static std::string CaptureRegistryUrlForDownload(const std::string& config_region,
                                                 const std::string& detected_region) {
  auto tmpdir = TempPath::CreateTempDir();

  std::string captured_url;
  auto registry = std::make_unique<ModelRegistryClient>(
      "eastus", fl::test::NullLog(), std::make_unique<RegionFallback>(fl::test::NullLog(), false),
      [&captured_url](const std::string& url) {
        captured_url = url;
        return MakeRegistryResponse(
            R"({"blobSasUri": "https://storage.blob.core.windows.net/container?sig=test"})");
      });

  auto mock_downloader = std::make_unique<MockBlobDownloader>();
  mock_downloader->expected_sas_uri = "https://storage.blob.core.windows.net/container?sig=test";
  mock_downloader->blobs_to_return = {{"config.json", 100}};

  auto manager = std::make_unique<DownloadManager>(
      tmpdir.string(), config_region, 64, fl::test::NullLog(), fl::test::TestTelemetrySink(),
      /*disable_region_fallback=*/false, std::move(registry), std::move(mock_downloader));

  ModelInfo info;
  info.model_id = "test-model:1";
  info.name = "test-model";
  info.uri = "azureml://registries/test/models/test-model/versions/1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "TestPublisher";
  info.detected_region = detected_region;

  manager->DownloadModel(info, nullptr);
  return captured_url;
}

TEST(DownloadManagerTest, Region_UsesDetectedRegionWhenConfigIsAuto) {
  auto url = CaptureRegistryUrlForDownload(/*config_region=*/"auto", /*detected_region=*/"westus2");
  EXPECT_TRUE(url.find("westus2.api.azureml.ms") != std::string::npos) << url;
}

TEST(DownloadManagerTest, Region_ExplicitConfigOverridesDetectedRegion) {
  auto url = CaptureRegistryUrlForDownload(/*config_region=*/"westeurope",
                                           /*detected_region=*/"westus2");
  EXPECT_TRUE(url.find("westeurope.api.azureml.ms") != std::string::npos) << url;
  EXPECT_TRUE(url.find("westus2.api.azureml.ms") == std::string::npos) << url;
}

TEST(DownloadManagerTest, Region_AutoConfigIsCaseInsensitive) {
  auto url = CaptureRegistryUrlForDownload(/*config_region=*/"AUTO", /*detected_region=*/"westus2");
  EXPECT_TRUE(url.find("westus2.api.azureml.ms") != std::string::npos) << url;
  EXPECT_TRUE(url.find("auto.api.azureml.ms") == std::string::npos) << url;
}

TEST(DownloadManagerTest, Region_FallsBackToDefaultRegistryRegionWhenNoConfigAndNoDetected) {
  auto url = CaptureRegistryUrlForDownload(/*config_region=*/"auto", /*detected_region=*/"");
  EXPECT_TRUE(url.find("centralus.api.azureml.ms") != std::string::npos) << url;
}

TEST(DownloadManagerTest, SkipsAlreadyCachedModel) {
  auto tmpdir = TempPath::CreateTempDir();
  RecordingDownloadTelemetry telemetry;
  auto manager = std::make_unique<DownloadManager>(tmpdir.string(), "eastus", 64, fl::test::NullLog(),
                                                   telemetry);

  ModelInfo info;
  info.model_id = "cached-model:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Publisher";

  // Pre-create the model directory (simulating an already-cached model)
  auto model_dir = fs::path(tmpdir.string()) / "Publisher" / "cached-model-1";
  fs::create_directories(model_dir);
  {
    std::ofstream f(model_dir / "inference_model.json");
    f << "{}";
  }

  float final_progress = 0;
  auto path = manager->DownloadModel(info, [&](float p) {
    final_progress = p;
    return 0;
  });

  EXPECT_EQ(path, model_dir.string());
  EXPECT_FLOAT_EQ(final_progress, 100.0f);
  ASSERT_EQ(telemetry.calls.size(), 1u);
  EXPECT_EQ(telemetry.calls[0].status, ActionStatus::kSkipped);
  EXPECT_EQ(telemetry.calls[0].model_id, info.model_id);
}

TEST(DownloadManagerTest, IsModelCachedReturnsFalseForMissing) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink());

  ModelInfo info;
  info.model_id = "nonexistent:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Publisher";

  EXPECT_FALSE(manager.IsModelCached(info));
}

TEST(DownloadManagerTest, IsModelCachedReturnsFalseForIncomplete) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink());

  ModelInfo info;
  info.model_id = "incomplete:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Publisher";

  // Create directory with download.tmp (incomplete)
  auto model_dir = fs::path(tmpdir.string()) / "Publisher" / "incomplete-1";
  fs::create_directories(model_dir);
  {
    std::ofstream f(model_dir / "download.tmp");
  }

  EXPECT_FALSE(manager.IsModelCached(info));
}

TEST(DownloadManagerTest, IsModelCachedReturnsTrueForComplete) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink());

  ModelInfo info;
  info.model_id = "complete:2";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Publisher";

  // Create directory with inference_model.json and no download.tmp (complete)
  auto model_dir = fs::path(tmpdir.string()) / "Publisher" / "complete-2";
  fs::create_directories(model_dir);
  {
    std::ofstream f(model_dir / "inference_model.json");
    f << "{}";
  }

  EXPECT_TRUE(manager.IsModelCached(info));
}

TEST(DownloadManagerTest, IsModelCachedReturnsFalseForEmptyDir) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink());

  ModelInfo info;
  info.model_id = "empty:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Publisher";

  // Create directory without inference_model.json — an empty directory from
  // a failed or aborted download should not be treated as cached.
  auto model_dir = fs::path(tmpdir.string()) / "Publisher" / "empty-1";
  fs::create_directories(model_dir);

  EXPECT_FALSE(manager.IsModelCached(info));
}

TEST(DownloadManagerTest, VersionSuffixConversion) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink());

  ModelInfo info;
  info.model_id = "mymodel:42";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Microsoft";

  auto path = manager.GetModelCachePath(info);
  // Path doesn't exist yet so should be empty
  EXPECT_TRUE(path.empty());

  // Create it and check the path uses '-' not ':'
  auto expected_dir = fs::path(tmpdir.string()) / "Microsoft" / "mymodel-42";
  fs::create_directories(expected_dir);

  path = manager.GetModelCachePath(info);
  EXPECT_EQ(path, expected_dir.string());
}

TEST(DownloadManagerTest, ThrowsOnEmptyUri) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink());

  ModelInfo info;
  info.model_id = "test:1";
  // No URI set

  EXPECT_THROW(manager.DownloadModel(info), fl::Exception);
}

// Concurrency: two threads downloading the same model must serialize so the second
// thread sees the cached result rather than re-downloading. Different models still
// proceed in parallel — covered by the unrelated-model test below.
TEST(DownloadManagerTest, ConcurrentDownloadsOfSameModelSerialize) {
  auto tmpdir = TempPath::CreateTempDir();

  auto registry = std::make_unique<ModelRegistryClient>(
      "eastus", fl::test::NullLog(), std::make_unique<RegionFallback>(fl::test::NullLog(), false),
      [](const std::string&) {
        return MakeRegistryResponse(R"({"blobSasUri": "https://storage.blob.core.windows.net/c?sig=test"})");
      });

  // Counting mock — increments an atomic on every DownloadBlob call.
  class CountingDownloader : public IBlobDownloader {
   public:
    std::atomic<int> download_calls{0};
    std::atomic<int> list_calls{0};

    std::vector<BlobItemInfo> ListBlobs(const std::string&) override {
      ++list_calls;
      return {{"variant-cpu/weights.bin", 16}};
    }

    void DownloadBlob(const std::string&, const std::string& blob_name,
                      const std::string& local_path, int,
                      BlobBytesWrittenFn bytes_written_cb,
                      std::atomic<bool>*) override {
      ++download_calls;

      auto parent = fs::path(local_path).parent_path();
      if (!parent.empty()) {
        fs::create_directories(parent);
      }
      std::ofstream f(local_path);
      f << "data for " << blob_name;
      if (bytes_written_cb) {
        bytes_written_cb(16);
      }
    }
  };

  auto counting = std::make_unique<CountingDownloader>();
  auto* counting_raw = counting.get();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink(),
                          /*disable_region_fallback=*/false, std::move(registry), std::move(counting));

  ModelInfo info;
  info.model_id = "concurrent-model:1";
  info.name = "concurrent-model";
  info.uri = "azureml://registries/test/models/concurrent-model/versions/1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Pub";

  constexpr int kThreadCount = 4;
  std::vector<std::thread> threads;
  std::vector<std::string> results(kThreadCount);
  std::atomic<int> exceptions{0};

  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back([&, i]() {
      try {
        results[i] = manager.DownloadModel(info);
      } catch (...) {
        ++exceptions;
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(exceptions.load(), 0);

  // Only ONE download should actually have run; the other three must have hit the cache.
  EXPECT_EQ(counting_raw->download_calls.load(), 1)
      << "Concurrent downloads of the same model must serialize and share the result.";

  // All four threads should report the same resolved path.
  for (int i = 1; i < kThreadCount; ++i) {
    EXPECT_EQ(results[i], results[0]);
  }
}

// All model downloads serialize through the process-wide download_mutex_, even
// for two *different* models. A concurrency probe records the peak number of
// downloads running at once; correct serialization keeps that peak at 1 (the
// second download can't enter until the first releases the mutex).
TEST(DownloadManagerTest, ModelDownloadsSerializeUnderGlobalLock) {
  auto tmpdir = TempPath::CreateTempDir();

  auto registry = std::make_unique<ModelRegistryClient>(
      "eastus", fl::test::NullLog(), std::make_unique<RegionFallback>(fl::test::NullLog(), false),
      [](const std::string&) {
        return MakeRegistryResponse(
            R"({"blobSasUri": "https://storage.blob.core.windows.net/c?sig=test"})");
      });

  // Tracks the peak number of downloads running at once. The global download
  // mutex must keep this at 1 even for different models.
  class ConcurrencyProbe : public IBlobDownloader {
   public:
    std::atomic<int> active{0};
    std::atomic<int> peak{0};

    std::vector<BlobItemInfo> ListBlobs(const std::string&) override {
      return {{"variant-cpu/weights.bin", 16}};
    }

    void DownloadBlob(const std::string&, const std::string& blob_name,
                      const std::string& local_path, int,
                      BlobBytesWrittenFn bytes_written_cb,
                      std::atomic<bool>*) override {
      int now = ++active;
      int prev = peak.load();
      while (now > prev && !peak.compare_exchange_weak(prev, now)) {
      }
      // Hold long enough that a second concurrent download would overlap here.
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      --active;

      auto parent = fs::path(local_path).parent_path();
      if (!parent.empty()) {
        fs::create_directories(parent);
      }
      std::ofstream f(local_path);
      f << "data for " << blob_name;
      if (bytes_written_cb) {
        bytes_written_cb(16);
      }
    }
  };

  auto probe = std::make_unique<ConcurrencyProbe>();
  auto* probe_raw = probe.get();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink(),
                          /*disable_region_fallback=*/false, std::move(registry), std::move(probe));

  auto make_info = [](const char* id, const char* publisher) {
    ModelInfo info;
    info.model_id = id;
    info.name = id;
    info.uri = std::string("azureml://registries/test/models/") + id + "/versions/1";
    info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = publisher;
    return info;
  };
  auto info_a = make_info("model-a:1", "PubA");
  auto info_b = make_info("model-b:1", "PubB");

  std::atomic<int> exceptions{0};
  std::thread t1([&] {
    try {
      manager.DownloadModel(info_a);
    } catch (...) {
      ++exceptions;
    }
  });
  std::thread t2([&] {
    try {
      manager.DownloadModel(info_b);
    } catch (...) {
      ++exceptions;
    }
  });
  t1.join();
  t2.join();

  EXPECT_EQ(exceptions.load(), 0);
  EXPECT_EQ(probe_raw->peak.load(), 1)
      << "The global download mutex must serialize all model downloads, even for different models.";
}

// Exercise the cross-process file-lock branch of DownloadModel that
// the in-process-only concurrency tests never reach. A second process (simulated
// here by holding the lock directly) is mid-download on the same model directory.
// DownloadModel must (1) observe the held lock, (2) block in WaitForDirectoryLock
// without holding the in-process download mutex, and (3) once the lock releases
// AND inference_model.json is present, return the cached result via the post-lock
// recheck WITHOUT re-downloading anything.
TEST(DownloadManagerTest, WaitsForCrossProcessLockThenServesCachedResult) {
  auto tmpdir = TempPath::CreateTempDir();

  // Registry + downloader that must stay untouched if the post-lock recheck works.
  auto registry = std::make_unique<ModelRegistryClient>(
      "eastus", fl::test::NullLog(), std::make_unique<RegionFallback>(fl::test::NullLog(), false),
      [](const std::string&) {
        return MakeRegistryResponse(
            R"({"blobSasUri": "https://storage.blob.core.windows.net/c?sig=test"})");
      });

  auto mock = std::make_unique<MockBlobDownloader>();
  mock->blobs_to_return = {{"weights.bin", 100}};  // non-empty: a stray download would be visible
  auto* mock_raw = mock.get();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink(),
                          /*disable_region_fallback=*/false, std::move(registry), std::move(mock));

  ModelInfo info;
  info.model_id = "wait-model:1";
  info.name = "wait-model";
  info.uri = "azureml://registries/test/models/wait-model/versions/1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Pub";

  // Simulate another process holding the model-directory lock mid-download.
  auto model_dir = fs::path(tmpdir.string()) / "Pub" / "wait-model-1";
  fs::create_directories(model_dir);
  auto held = CrossProcessFileLock::TryAcquireForDirectory(model_dir, fl::test::NullLog());
  ASSERT_NE(held, nullptr);

  std::atomic<bool> done{false};
  std::string result;
  std::thread worker([&] { result = manager.DownloadModel(info); done.store(true); });

  // The call must block on the cross-process lock rather than proceed to download.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_FALSE(done.load()) << "DownloadModel should block while another process holds the lock";

  // The "other process" finishes: publish inference_model.json, then release the lock.
  {
    std::ofstream(model_dir / "inference_model.json") << "{}";
  }
  held.reset();

  worker.join();

  EXPECT_TRUE(done.load());
  EXPECT_EQ(result, model_dir.string());
  EXPECT_TRUE(mock_raw->downloaded_blobs.empty())
      << "Model became available while waiting; the post-lock recheck must skip the download";
}

TEST(DownloadManagerTest, CanceledCrossProcessLockWaitRecordsCanceledWait) {
  auto tmpdir = TempPath::CreateTempDir();
  RecordingDownloadTelemetry telemetry;
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), telemetry);

  ModelInfo info;
  info.model_id = "canceled-wait-model:1";
  info.name = "canceled-wait-model";
  info.uri = "azureml://registries/test/models/canceled-wait-model/versions/1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Pub";

  auto model_dir = fs::path(tmpdir.string()) / "Pub" / "canceled-wait-model-1";
  fs::create_directories(model_dir);
  auto held = CrossProcessFileLock::TryAcquireForDirectory(model_dir, fl::test::NullLog());
  ASSERT_NE(held, nullptr);

  EXPECT_THROW(
      manager.DownloadModel(info, [](float) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 1;
      }),
      fl::Exception);

  ASSERT_EQ(telemetry.calls.size(), 1u);
  EXPECT_EQ(telemetry.calls[0].status, ActionStatus::kCanceled);
  EXPECT_EQ(telemetry.calls[0].download_wait_result, "Canceled");
  EXPECT_GE(telemetry.calls[0].lock_wait_ms, 10);
}

// HasInferenceModelJson must return false instead of throwing when the path
// it's asked about is not a directory (e.g. a regular file). Previously the
// underlying directory_iterator would throw filesystem_error.
TEST(DownloadManagerTest, IsModelCachedReturnsFalseWhenPathIsRegularFile) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink());

  ModelInfo info;
  info.model_id = "filemodel:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Pub";

  // Plant a regular file where the model directory would live.
  auto pub_dir = fs::path(tmpdir.string()) / "Pub";
  fs::create_directories(pub_dir);
  {
    std::ofstream f(pub_dir / "filemodel-1");
    f << "not a directory";
  }

  EXPECT_NO_THROW({
    EXPECT_FALSE(manager.IsModelCached(info));
  });
}

// ========================================================================
// End-to-end integration test — fetches catalog then downloads smallest model.
// Disabled by default. Run with: --gtest_also_run_disabled_tests
// ========================================================================

TEST(EndToEndTest, DISABLED_LiveCatalogAndDownload) {
  // 1. Fetch the full model list from the Azure catalog
  AllDevicesEpDetector ep;
  StderrLogger logger;
  AzureCatalogClient catalog("https://api.catalog.azureml.ms/asset-gallery/v1.0/models", "''", ep, logger);
  auto models = catalog.FetchAllModelInfos();

  ASSERT_GT(models.size(), 50u)
      << "Expected 50+ models from the public catalog, got " << models.size();

  // 2. Find the smallest chat-completion model by filesize_mb
  const ModelInfo* smallest = nullptr;
  int64_t smallest_size = std::numeric_limits<int64_t>::max();

  for (const auto& m : models) {
    auto task_it = m.string_properties.find(FOUNDRY_LOCAL_MODEL_PROP_TASK_STR);
    if (task_it == m.string_properties.end() || task_it->second != "chat-completion") {
      continue;
    }

    auto size_it = m.int_properties.find(FOUNDRY_LOCAL_MODEL_PROP_FILESIZE_MB_INT);
    if (size_it == m.int_properties.end()) {
      continue;
    }

    if (size_it->second < smallest_size) {
      smallest_size = size_it->second;
      smallest = &m;
    }
  }

  ASSERT_NE(smallest, nullptr)
      << "No chat-completion model with a filesize_mb property found in catalog";

  std::cout << "\n=== Live Catalog + Download Test ==="
            << "\nSelected model: " << smallest->name
            << "\nAlias:          " << smallest->alias
            << "\nSize (MB):      " << smallest_size
            << "\nURI:            " << smallest->uri
            << "\n====================================\n";

  // 3. Download the model — use build output dir so reruns skip the download
  auto cache_path = fs::path(__FILE__).parent_path().parent_path() / "build" / "test_cache";
  fs::create_directories(cache_path);
  DownloadManager dm(cache_path.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink());

  std::vector<float> progress_values;
  std::string local_path = dm.DownloadModel(*smallest, [&](float pct) {
    progress_values.push_back(pct);
    return 0;
  });

  // 4. Verify progress was reported and reached ~100%
  EXPECT_FALSE(progress_values.empty()) << "Progress callback was never called";
  if (!progress_values.empty()) {
    EXPECT_GE(progress_values.back(), 99.0f)
        << "Final progress should be ~100%, got " << progress_values.back();
  }

  // 5. Verify the download path exists on disk
  EXPECT_TRUE(fs::exists(local_path))
      << "Download path does not exist: " << local_path;

  // 6. Find inference_model.json — may be in root or a variant subdirectory
  fs::path inference_model_path;
  for (auto const& entry : fs::recursive_directory_iterator(local_path)) {
    if (entry.is_regular_file() && entry.path().filename() == "inference_model.json") {
      inference_model_path = entry.path();
      break;
    }
  }

  EXPECT_FALSE(inference_model_path.empty())
      << "inference_model.json not found anywhere under " << local_path;

  if (!inference_model_path.empty()) {
    // 7. Parse as JSON and validate the Name field
    std::string json_text = ReadFile(inference_model_path);
    nlohmann::json doc;
    EXPECT_NO_THROW(doc = nlohmann::json::parse(json_text))
        << "inference_model.json is not valid JSON";

    if (doc.is_object()) {
      EXPECT_TRUE(doc.contains("Name"))
          << "inference_model.json missing 'Name' field";
      if (doc.contains("Name")) {
        EXPECT_EQ(doc["Name"].get<std::string>(), smallest->model_id)
            << "Name mismatch between inference_model.json and catalog";
      }
    }
  }

  // 8. Verify no download.tmp signal file remains
  bool found_download_tmp = false;
  for (auto const& entry : fs::recursive_directory_iterator(local_path)) {
    if (entry.is_regular_file() && entry.path().filename() == "download.tmp") {
      found_download_tmp = true;
      break;
    }
  }
  EXPECT_FALSE(found_download_tmp)
      << "download.tmp signal file should be removed after a complete download";

  std::cout << "\nDownload path:          " << local_path
            << "\ninference_model.json:   " << inference_model_path.string()
            << "\nProgress callbacks:     " << progress_values.size()
            << "\n====================================\n";
}

// ========================================================================
// Path-injection hardening (H9) — DownloadManager::ComputeModelPath must
// reject catalog inputs that could escape the cache root or alias another
// publisher/model on disk.
// ========================================================================

TEST(DownloadManagerTest, RejectsParentEscapeInModelId) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink());

  ModelInfo info;
  info.model_id = "../evil:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Publisher";

  EXPECT_THROW(manager.GetModelCachePath(info), fl::Exception);
  EXPECT_THROW(manager.IsModelCached(info), fl::Exception);
}

TEST(DownloadManagerTest, RejectsBackslashInPublisher) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink());

  ModelInfo info;
  info.model_id = "test:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Pub\\..\\..\\evil";

  EXPECT_THROW(manager.GetModelCachePath(info), fl::Exception);
}

TEST(DownloadManagerTest, RejectsForwardSlashInPublisher) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink());

  ModelInfo info;
  info.model_id = "test:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Pub/sub";

  EXPECT_THROW(manager.GetModelCachePath(info), fl::Exception);
}

TEST(DownloadManagerTest, RejectsColonInBareModelId) {
  // model_id "drive:c:1" splits as bare="drive:c", version="1"; the bare half then
  // contains a stray ':' that would let a Windows drive letter slip through.
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink());

  ModelInfo info;
  info.model_id = "drive:c:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Publisher";

  EXPECT_THROW(manager.GetModelCachePath(info), fl::Exception);
}

TEST(DownloadManagerTest, RejectsTrailingDotInPublisher) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink());

  ModelInfo info;
  info.model_id = "test:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Publisher.";

  EXPECT_THROW(manager.GetModelCachePath(info), fl::Exception);
}

TEST(DownloadManagerTest, RejectsEmptyModelId) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink());

  ModelInfo info;
  info.model_id = "";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Publisher";

  EXPECT_THROW(manager.GetModelCachePath(info), fl::Exception);
}

TEST(DownloadManagerTest, AcceptsNormalModelIdAndPublisher) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog(), fl::test::TestTelemetrySink());

  ModelInfo info;
  info.model_id = "phi-3-mini:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Microsoft";

  // Should not throw. Path returned is empty until the directory is created on disk.
  EXPECT_NO_THROW(manager.GetModelCachePath(info));
  EXPECT_NO_THROW(manager.IsModelCached(info));
  EXPECT_FALSE(manager.IsModelCached(info));
}

// ========================================================================
// AzureBlobDownloader resume + cancel-cascade tests
// Use a subclass that overrides the protected GetBlobSize / DownloadChunkStreaming
// virtuals to bypass the real Azure SDK and simulate per-chunk behavior.
// ========================================================================

namespace {

class BlobIntegrityTransport final : public Azure::Core::Http::HttpTransport {
 public:
  std::vector<uint8_t> payload = std::vector<uint8_t>(32, 42);
  std::string checksum = "SaaDheEjzuynJH8eW6AEpQ==";
  bool include_checksum = true;
  bool saw_checksum_request = false;
  bool saw_etag_condition = false;

  std::unique_ptr<Azure::Core::Http::RawResponse> Send(Azure::Core::Http::Request& request,
                                                       const Azure::Core::Context&) override {
    using namespace Azure::Core::Http;
    const bool is_head = request.GetMethod() == HttpMethod::Head;
    auto response = std::make_unique<RawResponse>(1, 1, is_head ? HttpStatusCode::Ok : HttpStatusCode::PartialContent,
                                                  "OK");
    response->SetHeader("etag", "\"test-etag\"");
    response->SetHeader("last-modified", "Wed, 14 Oct 2026 00:00:00 GMT");
    response->SetHeader("x-ms-creation-time", "Wed, 14 Oct 2026 00:00:00 GMT");
    response->SetHeader("x-ms-server-encrypted", "true");
    response->SetHeader("x-ms-blob-type", "BlockBlob");
    response->SetHeader("content-length", std::to_string(payload.size()));
    response->SetBodyStream(std::make_unique<Azure::Core::IO::MemoryBodyStream>(nullptr, 0));
    if (!is_head) {
      const auto headers = request.GetHeaders();
      const auto checksum_header = headers.find("x-ms-range-get-content-md5");
      saw_checksum_request = checksum_header != headers.end() && checksum_header->second == "true";
      const auto etag_header = headers.find("if-match");
      saw_etag_condition = etag_header != headers.end() && etag_header->second == "\"test-etag\"";
      response->SetHeader("content-range", "bytes 0-31/32");
      if (include_checksum) {
        response->SetHeader("content-md5", checksum);
      }
      response->SetBodyStream(std::make_unique<Azure::Core::IO::MemoryBodyStream>(payload));
    }

    return response;
  }
};

/// Test double for AzureBlobDownloader. Overrides the protected virtuals so
/// chunked-download orchestration can be exercised without network I/O.
class FakeChunkAzureDownloader : public AzureBlobDownloader {
 public:
  int64_t blob_size = 0;

  /// Per-call hook. Receives the chunk offset and size plus a `sink` callback
  /// that forwards bytes to the file writer. Allowed to:
  /// - call `sink` zero or more times with strictly contiguous, cumulative
  ///   `size`-byte ranges to simulate a successful chunk
  /// - throw to simulate a transient failure (sink calls so far still hit disk)
  /// - sleep / poll cancellation
  std::function<void(int64_t offset, int64_t size,
                     const std::function<void(const uint8_t*, size_t)>& sink,
                     const std::function<bool()>& is_cancelled)>
      chunk_hook;

  std::atomic<int> chunk_call_count{0};
  std::mutex offsets_mutex;
  std::vector<int64_t> requested_offsets;

  using AzureBlobDownloader::AzureBlobDownloader;

  // AzureBlobDownloader now requires a logger reference. Tests don't care about
  // diagnostics, so default-construct against the shared null logger to keep the
  // many `FakeChunkAzureDownloader d;` sites terse.
  FakeChunkAzureDownloader() : AzureBlobDownloader(fl::test::NullLog()) {}

 protected:
  int64_t GetBlobSize(ChunkContext& /*ctx*/) override { return blob_size; }

  void DownloadChunkStreaming(ChunkContext& ctx, int64_t offset, int64_t size,
                              std::vector<uint8_t>& scratch,
                              const std::function<void(const uint8_t*, size_t)>& sink) override {
    chunk_call_count.fetch_add(1);
    {
      std::lock_guard<std::mutex> lock(offsets_mutex);
      requested_offsets.push_back(offset);
    }
    if (chunk_hook) {
      chunk_hook(offset, size, sink, [this, &ctx]() { return IsCancellationRequested(ctx); });
      return;
    }
    // Default: stream the chunk to the sink in scratch-sized pieces, filled
    // with the low byte of the offset for verification.
    if (scratch.size() < 64 * 1024) {
      scratch.resize(64 * 1024);
    }
    int64_t remaining = size;
    while (remaining > 0) {
      size_t to_emit =
          static_cast<size_t>(std::min<int64_t>(remaining, static_cast<int64_t>(scratch.size())));
      std::fill_n(scratch.begin(), to_emit, static_cast<uint8_t>(offset & 0xFF));
      sink(scratch.data(), to_emit);
      remaining -= static_cast<int64_t>(to_emit);
    }
  }
};

}  // namespace

TEST(AzureBlobDownloaderIntegrityTest, VerifiesServiceChecksumAndPinsRangeToETag) {
  auto tmpdir = TempPath::CreateTempDir();
  const auto local = tmpdir.path() / "blob.bin";
  auto transport = std::make_shared<BlobIntegrityTransport>();
  AzureBlobDownloader downloader(fl::test::NullLog(), transport);

  EXPECT_NO_THROW(downloader.DownloadBlob("https://test.blob.core.windows.net/container", "blob", local.string(), 1));
  EXPECT_TRUE(transport->saw_checksum_request);
  EXPECT_TRUE(transport->saw_etag_condition);
  EXPECT_FALSE(fs::exists(BlobDownloadState::GetStateFilePath(local)));
}

TEST(AzureBlobDownloaderIntegrityTest, RejectsMissingAndMismatchedServiceChecksums) {
  for (const bool include_checksum : {false, true}) {
    auto tmpdir = TempPath::CreateTempDir();
    const auto local = tmpdir.path() / "blob.bin";
    auto transport = std::make_shared<BlobIntegrityTransport>();
    transport->include_checksum = include_checksum;
    transport->checksum = "AAAAAAAAAAAAAAAAAAAAAA==";
    AzureBlobDownloader downloader(fl::test::NullLog(), transport);

    EXPECT_THROW(downloader.DownloadBlob("https://test.blob.core.windows.net/container", "blob", local.string(), 1),
                 fl::Exception);
    auto state = BlobDownloadState::LoadState("blob", local, 32, 2 * 1024 * 1024, 1, fl::test::NullLog());
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->completed_count, 0);
  }
}

TEST(AzureBlobDownloaderIntegrityTest, ReusesVerifiedFilesAndRepairsSameLengthCorruption) {
  auto tmpdir = TempPath::CreateTempDir();
  const auto local = tmpdir.path() / "blob.bin";
  auto transport = std::make_shared<BlobIntegrityTransport>();
  AzureBlobDownloader downloader(fl::test::NullLog(), transport);
  downloader.DownloadBlob("https://test.blob.core.windows.net/container", "blob", local.string(), 1);
  ASSERT_TRUE(fs::exists(BlobDownloadState::GetVerifiedStateFilePath(local)));

  MockBlobDownloader listing;
  listing.blobs_to_return = {{"blob.bin", 32, {}, "\"test-etag\""}};
  DownloadBlobsToDirectoryForTest(listing, "", tmpdir.string(), {});
  EXPECT_TRUE(listing.downloaded_blobs.empty());

  {
    std::fstream file(local, std::ios::binary | std::ios::in | std::ios::out);
    file.put('\0');
  }
  downloader.DownloadBlob("https://test.blob.core.windows.net/container", "blob", local.string(), 1);
  EXPECT_EQ(ReadFile(local), std::string(32, '*'));
  DownloadBlobsToDirectoryForTest(listing, "", tmpdir.string(), {});
  EXPECT_TRUE(listing.downloaded_blobs.empty());
}

TEST(AzureBlobDownloaderResumeTest, SkipsChunksAlreadyMarkedCompleteInSidecar) {
  auto tmpdir = TempPath::CreateTempDir();
  auto local = tmpdir.path() / "blob.bin";

  constexpr int32_t kChunkSize = 2 * 1024 * 1024;
  constexpr int32_t kNumChunks = 10;
  constexpr int64_t kBlobSize = static_cast<int64_t>(kNumChunks) * kChunkSize;

  // Pre-allocate the data file so the downloader takes the resume path.
  {
    std::ofstream f(local, std::ios::binary);
    f.seekp(kBlobSize - 1);
    f.put('\0');
  }
  // Pre-write a sidecar: chunks 0..4 done, 5..9 pending.
  {
    auto state = BlobDownloadState::CreateNew("blob", local, kBlobSize, kChunkSize, kNumChunks);
    for (int32_t i = 0; i < 5; ++i) {
      const auto digest = Md5(std::string(kChunkSize, '\0'));
      std::copy(digest.begin(), digest.end(), state->chunk_md5[static_cast<size_t>(i)].begin());
      state->MarkChunkComplete(i);
    }
    state->SaveState(fl::test::NullLog());
  }

  FakeChunkAzureDownloader d;
  d.blob_size = kBlobSize;

  d.DownloadBlob(/*sas_uri=*/"", "blob", local.string(), /*max_concurrency=*/2);

  EXPECT_EQ(d.chunk_call_count.load(), 5);
  std::sort(d.requested_offsets.begin(), d.requested_offsets.end());
  std::vector<int64_t> expected{5 * int64_t{kChunkSize}, 6 * int64_t{kChunkSize},
                                7 * int64_t{kChunkSize}, 8 * int64_t{kChunkSize},
                                9 * int64_t{kChunkSize}};
  EXPECT_EQ(d.requested_offsets, expected);

  // Sidecar should be gone on full success.
  EXPECT_FALSE(fs::exists(BlobDownloadState::GetStateFilePath(local)));
}

TEST(AzureBlobDownloaderResumeTest, IgnoresSidecarWhenDataFileTruncated) {
  // A valid sidecar marks chunks complete, but the data file was truncated (e.g.
  // an external cleanup) while the sidecar survived. The downloader must not trust
  // the sidecar — those "completed" chunks are no longer on disk — and must
  // re-download every chunk rather than leave them as zeros.
  auto tmpdir = TempPath::CreateTempDir();
  auto local = tmpdir.path() / "blob.bin";

  constexpr int32_t kChunkSize = 2 * 1024 * 1024;
  constexpr int32_t kNumChunks = 10;
  constexpr int64_t kBlobSize = static_cast<int64_t>(kNumChunks) * kChunkSize;

  // Sidecar claims chunks 0..4 are done.
  {
    auto state = BlobDownloadState::CreateNew("blob", local, kBlobSize, kChunkSize, kNumChunks);
    for (int32_t i = 0; i < 5; ++i) {
      state->MarkChunkComplete(i);
    }
    state->SaveState(fl::test::NullLog());
  }
  // ...but the data file is truncated, far smaller than kBlobSize.
  {
    std::ofstream f(local, std::ios::binary | std::ios::trunc);
    f << "truncated";
  }

  FakeChunkAzureDownloader d;
  d.blob_size = kBlobSize;

  d.DownloadBlob(/*sas_uri=*/"", "blob", local.string(), /*max_concurrency=*/2);

  // The stale sidecar is ignored: every chunk is downloaded, not just 5..9.
  EXPECT_EQ(d.chunk_call_count.load(), kNumChunks);
  EXPECT_FALSE(fs::exists(BlobDownloadState::GetStateFilePath(local)));
  EXPECT_EQ(fs::file_size(local), static_cast<uintmax_t>(kBlobSize));
}

TEST(AzureBlobDownloaderResumeTest, DownloadsAllChunksWhenSidecarMissing) {
  auto tmpdir = TempPath::CreateTempDir();
  auto local = tmpdir.path() / "blob.bin";

  constexpr int32_t kChunkSize = 2 * 1024 * 1024;
  constexpr int32_t kNumChunks = 4;
  constexpr int64_t kBlobSize = static_cast<int64_t>(kNumChunks) * kChunkSize;

  FakeChunkAzureDownloader d;
  d.blob_size = kBlobSize;

  d.DownloadBlob(/*sas_uri=*/"", "blob", local.string(), /*max_concurrency=*/4);

  EXPECT_EQ(d.chunk_call_count.load(), kNumChunks);
  EXPECT_FALSE(fs::exists(BlobDownloadState::GetStateFilePath(local)));
  // Local file is pre-allocated to blob_size during the first pass.
  EXPECT_TRUE(fs::exists(local));
  EXPECT_EQ(fs::file_size(local), static_cast<uintmax_t>(kBlobSize));
}

TEST(AzureBlobDownloaderResumeTest, RepairsSameLengthCorruptionInCompletedChunk) {
  auto tmpdir = TempPath::CreateTempDir();
  auto local = tmpdir.path() / "blob.bin";
  constexpr int32_t kChunkSize = 2 * 1024 * 1024;
  constexpr int64_t kBlobSize = 2 * int64_t{kChunkSize} + 17;
  {
    std::ofstream file(local, std::ios::binary);
    file << std::string(static_cast<size_t>(kBlobSize), '\0');
  }

  auto state = BlobDownloadState::CreateNew("blob", local, kBlobSize, kChunkSize, 3);
  for (int32_t i = 0; i < 3; ++i) {
    const auto digest = Md5(std::string(i == 2 ? 17 : kChunkSize, '\0'));
    std::copy(digest.begin(), digest.end(), state->chunk_md5[static_cast<size_t>(i)].begin());
    state->MarkChunkComplete(i);
  }

  ASSERT_TRUE(state->SaveState(fl::test::NullLog()));
  {
    std::fstream file(local, std::ios::binary | std::ios::in | std::ios::out);
    file.seekp(kChunkSize);
    file.put('X');
  }

  FakeChunkAzureDownloader downloader;
  downloader.blob_size = kBlobSize;
  downloader.DownloadBlob("", "blob", local.string(), 2);

  EXPECT_EQ(downloader.requested_offsets, std::vector<int64_t>{kChunkSize});
  EXPECT_FALSE(fs::exists(BlobDownloadState::GetStateFilePath(local)));
  EXPECT_EQ(fs::file_size(local), static_cast<uintmax_t>(kBlobSize));
}

TEST(AzureBlobDownloaderResumeTest, ChangedETagDiscardsCompletedChunks) {
  auto tmpdir = TempPath::CreateTempDir();
  auto local = tmpdir.path() / "blob.bin";
  std::ofstream(local, std::ios::binary) << std::string(32, '\0');
  auto state = BlobDownloadState::CreateNew("blob", local, 32, 2 * 1024 * 1024, 1);
  state->etag = "\"old-blob\"";
  const auto digest = Md5(std::string(32, '\0'));
  std::copy(digest.begin(), digest.end(), state->chunk_md5[0].begin());
  state->MarkChunkComplete(0);
  ASSERT_TRUE(state->SaveState(fl::test::NullLog()));

  FakeChunkAzureDownloader downloader;
  downloader.blob_size = 32;
  downloader.DownloadBlob("", "blob", local.string(), 1);

  EXPECT_EQ(downloader.chunk_call_count.load(), 1);
}

TEST(AzureBlobDownloaderResumeTest, ShortChunkCannotPublishPreallocatedFile) {
  auto tmpdir = TempPath::CreateTempDir();
  auto local = tmpdir.path() / "blob.bin";
  FakeChunkAzureDownloader downloader;
  downloader.blob_size = 32;
  downloader.chunk_hook = [](int64_t, int64_t, const auto& sink, const auto&) {
    const uint8_t byte = 42;
    sink(&byte, 1);
  };

  EXPECT_THROW(downloader.DownloadBlob("", "blob", local.string(), 1), fl::Exception);

  auto state = BlobDownloadState::LoadState("blob", local, 32, 2 * 1024 * 1024, 1, fl::test::NullLog());
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->completed_count, 0);
  EXPECT_EQ(fs::file_size(local), 32u);
}

TEST(AzureBlobDownloaderResumeTest, DetectsOnDiskCorruptionBeforeReportingSuccess) {
  auto tmpdir = TempPath::CreateTempDir();
  auto local = tmpdir.path() / "blob.bin";
  FakeChunkAzureDownloader downloader;
  downloader.blob_size = 32;
  auto corrupt_after_write = [&](int64_t) {
    std::fstream file(local, std::ios::binary | std::ios::in | std::ios::out);
    file.put('X');
  };

  EXPECT_THROW(downloader.DownloadBlob("", "blob", local.string(), 1, corrupt_after_write), fl::Exception);

  auto state = BlobDownloadState::LoadState("blob", local, 32, 2 * 1024 * 1024, 1, fl::test::NullLog());
  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->completed_count, 0);

  downloader.DownloadBlob("", "blob", local.string(), 1);
  EXPECT_FALSE(fs::exists(BlobDownloadState::GetStateFilePath(local)));
}

TEST(AzureBlobDownloaderResumeTest, PersistsSidecarOnChunkFailure) {
  auto tmpdir = TempPath::CreateTempDir();
  auto local = tmpdir.path() / "blob.bin";

  constexpr int32_t kChunkSize = 2 * 1024 * 1024;
  constexpr int32_t kNumChunks = 10;
  constexpr int64_t kBlobSize = static_cast<int64_t>(kNumChunks) * kChunkSize;

  FakeChunkAzureDownloader d;
  d.blob_size = kBlobSize;
  // Fail when we see the offset of chunk 4 (specifically chosen so several
  // chunks land before the failing one across threads).
  constexpr int64_t kFailOffset = 4 * int64_t{kChunkSize};
  d.chunk_hook = [&](int64_t offset, int64_t size,
                     const std::function<void(const uint8_t*, size_t)>& sink,
                     const std::function<bool()>& /*is_cancelled*/) {
    if (offset == kFailOffset) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "simulated chunk failure");
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size), static_cast<uint8_t>(offset & 0xFF));
    sink(buf.data(), buf.size());
  };

  EXPECT_THROW(
      d.DownloadBlob(/*sas_uri=*/"", "blob", local.string(), /*max_concurrency=*/2),
      fl::Exception);

  // The sidecar should be persisted so a subsequent call can resume.
  EXPECT_TRUE(fs::exists(BlobDownloadState::GetStateFilePath(local)));

  // Verify the persisted sidecar records partial progress — some chunks completed
  // before the failure, but not all — so a future resume can skip the ones already
  // done and re-fetch only the rest.
  auto retry_state = BlobDownloadState::LoadState("blob", local, kBlobSize, kChunkSize, kNumChunks,
                                                  fl::test::NullLog());
  ASSERT_NE(retry_state, nullptr);
  EXPECT_GT(retry_state->completed_count, 0);
  EXPECT_LT(retry_state->completed_count, kNumChunks);
}

// Persist explicit incomplete state before pre-allocation, including when the first chunk never completes.
TEST(AzureBlobDownloaderResumeTest, SidecarExistsBeforeFirstChunkCompletes) {
  auto tmpdir = TempPath::CreateTempDir();
  auto local = tmpdir.path() / "blob.bin";

  constexpr int32_t kChunkSize = 2 * 1024 * 1024;
  constexpr int32_t kNumChunks = 100;  // far above the per-save chunk interval
  constexpr int64_t kBlobSize = static_cast<int64_t>(kNumChunks) * kChunkSize;

  FakeChunkAzureDownloader d;
  d.blob_size = kBlobSize;

  auto sidecar = BlobDownloadState::GetStateFilePath(local);
  std::atomic<bool> recorded{false};
  std::atomic<bool> sidecar_present_at_first_chunk{false};
  d.chunk_hook = [&](int64_t /*offset*/, int64_t /*size*/,
                     const std::function<void(const uint8_t*, size_t)>& /*sink*/,
                     const std::function<bool()>&) {
    if (!recorded.exchange(true)) {
      // First chunk callback: CreateNew + the initial SaveState + Open() have
      // all run, so the sidecar must already exist. Abort before any periodic
      // save to mimic an early interruption.
      sidecar_present_at_first_chunk.store(fs::exists(sidecar));
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "stop after first chunk");
    }
  };

  EXPECT_THROW(d.DownloadBlob(/*sas_uri=*/"", "blob", local.string(), /*max_concurrency=*/1),
               fl::Exception);

  EXPECT_TRUE(sidecar_present_at_first_chunk.load())
      << "Sidecar must exist before any chunk completes so an early crash stays resumable.";
  EXPECT_TRUE(fs::exists(sidecar));
  EXPECT_TRUE(fs::exists(local));
  EXPECT_EQ(fs::file_size(local), static_cast<uintmax_t>(kBlobSize));
}

TEST(AzureBlobDownloaderResumeTest, CleansUpSidecarOnEmptyBlob) {
  auto tmpdir = TempPath::CreateTempDir();
  auto local = tmpdir.path() / "empty.bin";
  // Plant a stale sidecar.
  {
    std::ofstream f(BlobDownloadState::GetStateFilePath(local), std::ios::binary);
    f << "stale";
  }

  FakeChunkAzureDownloader d;
  d.blob_size = 0;  // empty

  d.DownloadBlob(/*sas_uri=*/"", "empty", local.string(), /*max_concurrency=*/4);

  EXPECT_TRUE(fs::exists(local));
  EXPECT_EQ(fs::file_size(local), 0u);
  EXPECT_FALSE(fs::exists(BlobDownloadState::GetStateFilePath(local)));
  EXPECT_EQ(d.chunk_call_count.load(), 0);
}

TEST(AzureBlobDownloaderResumeTest, ChunkFailureCancelsInFlightPeersFast) {
  auto tmpdir = TempPath::CreateTempDir();
  auto local = tmpdir.path() / "blob.bin";

  constexpr int32_t kChunkSize = 2 * 1024 * 1024;
  constexpr int32_t kNumChunks = 10;
  constexpr int64_t kBlobSize = static_cast<int64_t>(kNumChunks) * kChunkSize;
  constexpr int64_t kFailOffset = 4 * int64_t{kChunkSize};

  FakeChunkAzureDownloader d;
  d.blob_size = kBlobSize;
  // The failing chunk throws fast. Every other chunk sleeps for up to 5 s in
  // 50-ms slices, polling cancellation. If linked cancellation works, they
  // observe it within one slice of the failure and exit promptly.
  d.chunk_hook = [](int64_t offset, int64_t size,
                    const std::function<void(const uint8_t*, size_t)>& sink,
                    const std::function<bool()>& is_cancelled) {
    if (offset == kFailOffset) {
      // Give other workers a moment to enter their sleep loop before we throw,
      // so we're meaningfully testing the cancel-while-in-flight path.
      std::this_thread::sleep_for(std::chrono::milliseconds(75));
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "simulated chunk failure");
    }
    for (int i = 0; i < 100; ++i) {
      if (is_cancelled()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "cancelled mid-chunk");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    sink(buf.data(), buf.size());
  };

  auto start = std::chrono::steady_clock::now();
  EXPECT_THROW(
      d.DownloadBlob(/*sas_uri=*/"", "blob", local.string(), /*max_concurrency=*/kNumChunks),
      fl::Exception);
  auto elapsed = std::chrono::steady_clock::now() - start;
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

  // Without cancellation, the slow chunks would sleep ~5 s. With it, they
  // should all exit within a few hundred ms of the failure (well under 2 s).
  EXPECT_LT(elapsed_ms, 2000)
      << "Cancel-cascade should drain in-flight peers fast; took " << elapsed_ms << " ms";
}

TEST(AzureBlobDownloaderResumeTest, UserCancelDrainsInFlightPeersFast) {
  auto tmpdir = TempPath::CreateTempDir();
  auto local = tmpdir.path() / "blob.bin";

  constexpr int32_t kChunkSize = 2 * 1024 * 1024;
  constexpr int32_t kNumChunks = 10;
  constexpr int64_t kBlobSize = static_cast<int64_t>(kNumChunks) * kChunkSize;

  FakeChunkAzureDownloader d;
  d.blob_size = kBlobSize;

  // Chunk 0 is the cancel trigger; chunks 1..9 are the in-flight peers. The peers
  // announce themselves and then sleep up to 5 s in 50-ms slices, polling the
  // Azure-context cancellation. Chunk 0 waits until every peer is parked in that
  // sleep loop before it completes, so no peer is at the worker top-of-loop to
  // observe the shared cancel flag directly -- the only way they can exit
  // promptly is the azure_ctx.Cancel() driven by the user-cancel throw.
  std::atomic<int> peers_parked{0};
  d.chunk_hook = [&peers_parked](int64_t offset, int64_t size,
                                 const std::function<void(const uint8_t*, size_t)>& sink,
                                 const std::function<bool()>& is_cancelled) {
    if (offset == 0) {
      for (int i = 0; i < 400 && peers_parked.load() < kNumChunks - 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
      sink(buf.data(), buf.size());
      return;
    }
    peers_parked.fetch_add(1);
    for (int i = 0; i < 100; ++i) {
      if (is_cancelled()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "cancelled mid-chunk");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size), 0);
    sink(buf.data(), buf.size());
  };

  // Mirror per_chunk_progress: the first progress callback cancels by setting the
  // shared flag and throwing.
  std::atomic<bool> cancelled{false};
  BlobBytesWrittenFn cancel_on_first_progress = [&cancelled](int64_t /*bytes*/) {
    cancelled.store(true, std::memory_order_relaxed);
    FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "download cancelled by user callback return value");
  };

  auto start = std::chrono::steady_clock::now();
  EXPECT_THROW(d.DownloadBlob(/*sas_uri=*/"", "blob", local.string(), /*max_concurrency=*/kNumChunks,
                              cancel_on_first_progress, &cancelled),
               fl::Exception);
  auto elapsed = std::chrono::steady_clock::now() - start;
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

  // Without routing the user-cancel throw through azure_ctx.Cancel(), the parked
  // peers would each sleep their full ~5 s before noticing. With it, they exit
  // within a slice or two (well under 2 s).
  EXPECT_LT(elapsed_ms, 2000)
      << "User-cancel should drain in-flight peers fast; took " << elapsed_ms << " ms";
}
