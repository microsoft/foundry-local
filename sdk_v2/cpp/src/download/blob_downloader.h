// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace fl {

class ILogger;

/// Progress callback: percent is 0.0 to 100.0. Return 0 to continue, non-zero to cancel.
using DownloadProgressFn = std::function<int(float percent)>;

/// Per-chunk progress callback used during a single blob download.
/// Receives the number of bytes written so far for that blob.
using BlobBytesWrittenFn = std::function<void(int64_t bytes_written)>;

/// Options for blob download operations.
struct BlobDownloadOptions {
  /// Path prefix filter — only download blobs whose name starts with this.
  std::string path_prefix;

  /// Maximum concurrent chunk downloads per blob. Default matches C# desktop.
  int max_concurrency = 64;

  /// Progress callback (optional). Return non-zero to cancel the download.
  DownloadProgressFn progress;
};

/// Information about a single blob in a container.
struct BlobItemInfo {
  std::string name;
  int64_t content_length = 0;
};

/// Interface for blob download operations. Enables testing via mock implementations.
class IBlobDownloader {
 public:
  virtual ~IBlobDownloader() = default;

  /// List all blobs in the container at the given SAS URI.
  virtual std::vector<BlobItemInfo> ListBlobs(const std::string& sas_uri) = 0;

  /// Download a single blob to a local file path.
  /// The sas_uri is the container SAS URI; blob_name identifies the blob within it.
  /// If bytes_written_cb is provided, it is called after each chunk with the cumulative byte count.
  /// If cancelled is non-null, the download checks the flag and aborts promptly when set.
  virtual void DownloadBlob(const std::string& sas_uri,
                            const std::string& blob_name,
                            const std::string& local_path,
                            int max_concurrency,
                            BlobBytesWrittenFn bytes_written_cb = nullptr,
                            std::atomic<bool>* cancelled = nullptr) = 0;
};

/// Azure Storage Blobs SDK-based implementation of IBlobDownloader.
///
/// Implements resumable downloads: a `<file>.dlstate` sidecar tracks which 2 MB
/// chunks have completed, and DownloadBlob picks up where a prior aborted run
/// left off. A linked cancellation token cascades the first chunk-level
/// failure to every other in-flight chunk so the worker pool drains quickly.
///
/// Chunks stream from the blob client into the local file in ~64 KB pieces
/// via a sink callback, so each worker holds a single 64 KB scratch buffer
/// instead of allocating a full chunk's worth of bytes per request. This
/// caps peak memory at roughly `max_concurrency * 64 KB` regardless of how
/// large the blob or the chunk size is.
class AzureBlobDownloader : public IBlobDownloader {
 public:
  /// `logger` receives diagnostics only (state-file save/load events). It is required:
  /// the orchestrator always has a logger, so there is no optional/null case to handle.
  explicit AzureBlobDownloader(ILogger& logger);

  std::vector<BlobItemInfo> ListBlobs(const std::string& sas_uri) override;

  void DownloadBlob(const std::string& sas_uri,
                    const std::string& blob_name,
                    const std::string& local_path,
                    int max_concurrency,
                    BlobBytesWrittenFn bytes_written_cb = nullptr,
                    std::atomic<bool>* cancelled = nullptr) override;

 protected:
  /// Opaque per-blob context. Defined in `blob_downloader.cc`; holds the Azure
  /// SDK BlobClient + Context pointers used by the production virtuals.
  struct ChunkContext;

  /// Return the blob size in bytes. Production calls `BlobClient::GetProperties`.
  virtual int64_t GetBlobSize(ChunkContext& ctx);

  /// Read `size` bytes starting at `offset` from the blob and forward them
  /// piecewise to `sink`. Pulls from the blob client referenced by `ctx`.
  ///
  /// `scratch` is a per-worker reusable buffer (default 64 KB). `sink` must be
  ///  invoked with strictly contiguous ranges; the cumulative byte count
  ///  delivered to `sink` must equal `size` on success.
  ///
  /// Must throw on failure. Implementations should observe the cancellation
  /// flag accessible via `ctx` and exit promptly when cancellation is requested.
  virtual void DownloadChunkStreaming(ChunkContext& ctx,
                                       int64_t offset,
                                       int64_t size,
                                       std::vector<uint8_t>& scratch,
                                       const std::function<void(const uint8_t*, size_t)>& sink);

  /// Reports whether cooperative cancellation has been requested for this
  /// download. The orchestrator calls `Azure::Core::Context::Cancel()` after a
  /// sibling chunk fails or on external cancellation, and the Azure SDK
  /// interrupts in-flight transfers as a result.
  bool IsCancellationRequested(const ChunkContext& ctx) const;

 private:
  ILogger& logger_;
};

/// High-level download function: enumerate, filter, and download all blobs from a SAS URI.
/// Handles safetensors optimization, path prefix filtering, and progress reporting.
/// Throws fl::Exception on failure.
void DownloadBlobsToDirectory(IBlobDownloader& downloader,
                              const std::string& sas_uri,
                              const std::string& output_directory,
                              const BlobDownloadOptions& options);

}  // namespace fl
