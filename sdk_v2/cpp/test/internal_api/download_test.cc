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
#include "download/blob_download_state.h"
#include "download/blob_downloader.h"
#include "platform/cross_process_file_lock.h"
#include "download/download_manager.h"
#include "download/inference_model_writer.h"
#include "download/model_registry_client.h"
#include "ep_detection/ep_detector.h"
#include "exception.h"
#include "logger.h"
#include "model_info.h"
#include "test_helpers.h"
#include "util/path_safety.h"
#include "util/region_fallback.h"
#include <foundry_local/foundry_local_c.h>
#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
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
    f << "mock content for " << blob_name;

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
  DownloadBlobsToDirectory(mock, "https://test.blob/container?sig=x", tmpdir.string(), opts);

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
  DownloadBlobsToDirectory(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);

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
  DownloadBlobsToDirectory(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);

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

  DownloadBlobsToDirectory(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);

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
  DownloadBlobsToDirectory(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);

  EXPECT_TRUE(mock.downloaded_blobs.empty());
}

// ========================================================================
// Skip-existing (Increment 1: resumable downloads)
// ========================================================================

TEST(BlobDownloadTest, SkipsExistingFilesWithCorrectSize) {
  auto tmpdir = TempPath::CreateTempDir();
  // Pre-create one of the blobs at the expected size on disk.
  std::ofstream(tmpdir.path() / "weights.safetensors") << std::string(1000, 'X');

  MockBlobDownloader mock;
  mock.blobs_to_return = {
      {"weights.safetensors", 1000},
      {"config.json", 100},
  };

  BlobDownloadOptions opts;
  DownloadBlobsToDirectory(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);

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
  DownloadBlobsToDirectory(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);

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
      {"already.bin", 500},
      {"missing.bin", 1500},
  };

  std::vector<float> progress_values;
  BlobDownloadOptions opts;
  opts.progress = [&](float pct) {
    progress_values.push_back(pct);
    return 0;
  };

  DownloadBlobsToDirectory(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);

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
      {"a.bin", 100},
      {"b.bin", 200},
  };

  std::vector<float> progress_values;
  BlobDownloadOptions opts;
  opts.progress = [&](float pct) {
    progress_values.push_back(pct);
    return 0;
  };

  DownloadBlobsToDirectory(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);

  EXPECT_TRUE(mock.downloaded_blobs.empty());
  ASSERT_FALSE(progress_values.empty());
  EXPECT_FLOAT_EQ(progress_values.front(), 100.0f);
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
  EXPECT_THROW(DownloadBlobsToDirectory(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts),
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
  EXPECT_THROW(DownloadBlobsToDirectory(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts),
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
  EXPECT_THROW(DownloadBlobsToDirectory(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts),
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
    DownloadBlobsToDirectory(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);
    FAIL() << "Expected fl::Exception to be thrown";
  } catch (const fl::Exception& e) {
    EXPECT_EQ(e.code(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  }

  EXPECT_EQ(mock.download_count, 0)
      << "No blobs should have been downloaded — cancellation fired at 0% before any blob started";
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
    DownloadBlobsToDirectory(mock, "https://test.blob/c?sig=x", tmpdir.string(), opts);
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

TEST(DownloadManagerTest, FullDownloadFlow) {
  auto tmpdir = TempPath::CreateTempDir();

  auto manager = std::make_unique<DownloadManager>(tmpdir.string(), "eastus", 64, fl::test::NullLog());

  // Mock the registry client
  auto registry = std::make_unique<ModelRegistryClient>(
      "eastus", fl::test::NullLog(), std::make_unique<RegionFallback>(fl::test::NullLog(), false),
      [](const std::string&) {
        return MakeRegistryResponse(
            R"({"blobSasUri": "https://storage.blob.core.windows.net/container?sig=test"})");
      });
  manager->SetModelRegistryClient(std::move(registry));

  // Mock the blob downloader
  auto mock_downloader = std::make_unique<MockBlobDownloader>();
  mock_downloader->expected_sas_uri =
      "https://storage.blob.core.windows.net/container?sig=test";
  mock_downloader->blobs_to_return = {
      {"weights.safetensors", 1024},
      {"config.json", 100},
  };
  manager->SetBlobDownloader(std::move(mock_downloader));

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
}

// --- Region resolution: detected region drives the download endpoint ---

// Run one download and return the registry URL the manager hit.
static std::string CaptureRegistryUrlForDownload(const std::string& config_region,
                                                 const std::string& detected_region) {
  auto tmpdir = TempPath::CreateTempDir();
  auto manager =
      std::make_unique<DownloadManager>(tmpdir.string(), config_region, 64, fl::test::NullLog());

  std::string captured_url;
  auto registry = std::make_unique<ModelRegistryClient>(
      "eastus", fl::test::NullLog(), std::make_unique<RegionFallback>(fl::test::NullLog(), false),
      [&captured_url](const std::string& url) {
        captured_url = url;
        return MakeRegistryResponse(
            R"({"blobSasUri": "https://storage.blob.core.windows.net/container?sig=test"})");
      });
  manager->SetModelRegistryClient(std::move(registry));

  auto mock_downloader = std::make_unique<MockBlobDownloader>();
  mock_downloader->expected_sas_uri = "https://storage.blob.core.windows.net/container?sig=test";
  mock_downloader->blobs_to_return = {{"config.json", 100}};
  manager->SetBlobDownloader(std::move(mock_downloader));

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
  auto manager = std::make_unique<DownloadManager>(tmpdir.string(), "eastus", 64, fl::test::NullLog());

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
}

TEST(DownloadManagerTest, IsModelCachedReturnsFalseForMissing) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

  ModelInfo info;
  info.model_id = "nonexistent:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Publisher";

  EXPECT_FALSE(manager.IsModelCached(info));
}

TEST(DownloadManagerTest, IsModelCachedReturnsFalseForIncomplete) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

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
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

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
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

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
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

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
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

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
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

  auto registry = std::make_unique<ModelRegistryClient>(
      "eastus", fl::test::NullLog(), std::make_unique<RegionFallback>(fl::test::NullLog(), false),
      [](const std::string&) {
        return MakeRegistryResponse(R"({"blobSasUri": "https://storage.blob.core.windows.net/c?sig=test"})");
      });
  manager.SetModelRegistryClient(std::move(registry));

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
  manager.SetBlobDownloader(std::move(counting));

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
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

  auto registry = std::make_unique<ModelRegistryClient>(
      "eastus", fl::test::NullLog(), std::make_unique<RegionFallback>(fl::test::NullLog(), false),
      [](const std::string&) {
        return MakeRegistryResponse(
            R"({"blobSasUri": "https://storage.blob.core.windows.net/c?sig=test"})");
      });
  manager.SetModelRegistryClient(std::move(registry));

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
  manager.SetBlobDownloader(std::move(probe));

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
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

  // Registry + downloader that must stay untouched if the post-lock recheck works.
  auto registry = std::make_unique<ModelRegistryClient>(
      "eastus", fl::test::NullLog(), std::make_unique<RegionFallback>(fl::test::NullLog(), false),
      [](const std::string&) {
        return MakeRegistryResponse(
            R"({"blobSasUri": "https://storage.blob.core.windows.net/c?sig=test"})");
      });
  manager.SetModelRegistryClient(std::move(registry));

  auto mock = std::make_unique<MockBlobDownloader>();
  mock->blobs_to_return = {{"weights.bin", 100}};  // non-empty: a stray download would be visible
  auto* mock_raw = mock.get();
  manager.SetBlobDownloader(std::move(mock));

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

// HasInferenceModelJson must return false instead of throwing when the path
// it's asked about is not a directory (e.g. a regular file). Previously the
// underlying directory_iterator would throw filesystem_error.
TEST(DownloadManagerTest, IsModelCachedReturnsFalseWhenPathIsRegularFile) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

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
  AzureCatalogClient catalog("https://ai.azure.com/api/eastus/ux/v1.0", "''", ep, logger);
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
  DownloadManager dm(cache_path.string(), "eastus", 64, fl::test::NullLog());

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
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

  ModelInfo info;
  info.model_id = "../evil:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Publisher";

  EXPECT_THROW(manager.GetModelCachePath(info), fl::Exception);
  EXPECT_THROW(manager.IsModelCached(info), fl::Exception);
}

TEST(DownloadManagerTest, RejectsBackslashInPublisher) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

  ModelInfo info;
  info.model_id = "test:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Pub\\..\\..\\evil";

  EXPECT_THROW(manager.GetModelCachePath(info), fl::Exception);
}

TEST(DownloadManagerTest, RejectsForwardSlashInPublisher) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

  ModelInfo info;
  info.model_id = "test:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Pub/sub";

  EXPECT_THROW(manager.GetModelCachePath(info), fl::Exception);
}

TEST(DownloadManagerTest, RejectsColonInBareModelId) {
  // model_id "drive:c:1" splits as bare="drive:c", version="1"; the bare half then
  // contains a stray ':' that would let a Windows drive letter slip through.
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

  ModelInfo info;
  info.model_id = "drive:c:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Publisher";

  EXPECT_THROW(manager.GetModelCachePath(info), fl::Exception);
}

TEST(DownloadManagerTest, RejectsTrailingDotInPublisher) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

  ModelInfo info;
  info.model_id = "test:1";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Publisher.";

  EXPECT_THROW(manager.GetModelCachePath(info), fl::Exception);
}

TEST(DownloadManagerTest, RejectsEmptyModelId) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

  ModelInfo info;
  info.model_id = "";
  info.string_properties[FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR] = "Publisher";

  EXPECT_THROW(manager.GetModelCachePath(info), fl::Exception);
}

TEST(DownloadManagerTest, AcceptsNormalModelIdAndPublisher) {
  auto tmpdir = TempPath::CreateTempDir();
  DownloadManager manager(tmpdir.string(), "eastus", 64, fl::test::NullLog());

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

// Regression: the sidecar must reach disk before the data file is pre-allocated,
// not only after save_interval chunks. Open() pre-allocates the file to full
// size, and IsDownloadNeeded treats "full-size data file + no sidecar" as a
// completed download. So a crash in the window between pre-allocation and the
// first periodic save would otherwise leave a full-size, empty file that the
// next run skips — silently serving zeros. Verify a sidecar is already present
// the moment the first chunk is requested.
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
