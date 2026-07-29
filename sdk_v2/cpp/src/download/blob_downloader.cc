// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "download/blob_downloader.h"
#include "download/blob_download_state.h"
#include "download/file_writer.h"
#include "exception.h"
#include "logger.h"
#include "util/path_safety.h"
#include "util/string_utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <azure/core/context.hpp>
#include <azure/storage/blobs.hpp>

namespace fl {

namespace {

/// Streaming buffer size used by the production chunk downloader. Matches the
/// 64 KB-ish granularity Stream.CopyTo uses in .NET, capping per-worker peak
/// memory at this many bytes regardless of chunk size.
constexpr size_t kStreamingBufferBytes = 64 * 1024;

}  // namespace

// ========================================================================
// AzureBlobDownloader — real Azure Storage SDK implementation
// ========================================================================

/// Per-blob shared state passed to the protected virtuals. Both members are
/// references to objects the orchestrator owns on the stack for the lifetime of
/// the download, so they are never null. `blob_client` is const because every
/// call routed through it (GetProperties / Download) is a const SDK operation.
/// `azure_ctx` is const here because the virtuals only *observe* cancellation
/// (IsCancelled, and handing the context to SDK reads); the orchestrator
/// initiates cancellation by calling Cancel() on the owning Context directly,
/// not through this view.
struct AzureBlobDownloader::ChunkContext {
  const Azure::Storage::Blobs::BlobClient& blob_client;
  const Azure::Core::Context& azure_ctx;
};

AzureBlobDownloader::AzureBlobDownloader(ILogger& logger) : logger_(logger) {}

std::vector<BlobItemInfo> AzureBlobDownloader::ListBlobs(const std::string& sas_uri) {
  try {
    auto container_client = Azure::Storage::Blobs::BlobContainerClient(sas_uri);
    std::vector<BlobItemInfo> items;

    for (auto page = container_client.ListBlobs(); page.HasPage(); page.MoveToNextPage()) {
      for (const auto& blob : page.Blobs) {
        BlobItemInfo info;
        info.name = blob.Name;
        info.content_length = blob.BlobSize;
        items.push_back(std::move(info));
      }
    }

    return items;
  } catch (const Azure::Core::RequestFailedException& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK,
             std::string("failed to list blobs: ") + e.what());
  }
}

int64_t AzureBlobDownloader::GetBlobSize(ChunkContext& ctx) {
  auto props = ctx.blob_client.GetProperties({}, ctx.azure_ctx).Value;
  return props.BlobSize;
}

bool AzureBlobDownloader::IsCancellationRequested(const ChunkContext& ctx) const {
  return ctx.azure_ctx.IsCancelled();
}

void AzureBlobDownloader::DownloadChunkStreaming(
    ChunkContext& ctx, int64_t offset, int64_t size, std::vector<uint8_t>& scratch,
    const std::function<void(const uint8_t*, size_t)>& sink) {
  Azure::Storage::Blobs::DownloadBlobOptions range_opts;
  range_opts.Range = Azure::Core::Http::HttpRange{offset, size};
  auto result = ctx.blob_client.Download(range_opts, ctx.azure_ctx);
  auto& body_stream = *result.Value.BodyStream;

  if (scratch.size() < kStreamingBufferBytes) {
    scratch.resize(kStreamingBufferBytes);
  }

  int64_t remaining = size;
  while (remaining > 0) {
    size_t to_read = static_cast<size_t>(std::min<int64_t>(remaining, static_cast<int64_t>(scratch.size())));
    size_t got = body_stream.Read(scratch.data(), to_read, ctx.azure_ctx);
    if (got == 0) {
      // Zero-byte read before reaching `size` means the server closed early.
      // Treat as a hard error rather than silently writing a truncated chunk.
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "short read from blob stream at offset " + std::to_string(offset) + ": got " +
                   std::to_string(size - remaining) + " of " + std::to_string(size) + " bytes");
    }
    sink(scratch.data(), got);
    remaining -= static_cast<int64_t>(got);
  }
}

namespace {

/// Create (truncate to) a zero-byte file at `local_path`, throwing on failure.
///
/// Used only for the empty-blob case below: a 0-length blob has no chunks to
/// stream, so there is nothing for `FileWriter::Open` to pre-allocate — we just
/// materialize the empty file. The chunked path's pre-allocation lives in `Open`.
void EnsureEmptyBlobFile(const std::string& local_path) {
  std::ofstream f(local_path, std::ios::binary);
  if (!f.is_open()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "failed to create empty blob file: " + local_path);
  }
}

}  // namespace

void AzureBlobDownloader::DownloadBlob(const std::string& sas_uri,
                                       const std::string& blob_name,
                                       const std::string& local_path,
                                       int max_concurrency,
                                       BlobBytesWrittenFn bytes_written_cb,
                                       std::atomic<bool>* cancelled) {
  try {
    // Configure retry at the SDK level instead of a manual retry loop.
    // Exponential backoff: 2s initial delay, 30s cap, generous retry count.
    Azure::Storage::Blobs::BlobClientOptions client_options;
    client_options.Retry.MaxRetries = 10;
    client_options.Retry.RetryDelay = std::chrono::milliseconds{2000};
    client_options.Retry.MaxRetryDelay = std::chrono::milliseconds{30000};
    // Add 429 (TooManyRequests) to the default retryable status codes.
    // Defaults already include: 408, 500, 502, 503, 504.
    client_options.Retry.StatusCodes.insert(Azure::Core::Http::HttpStatusCode::TooManyRequests);

    auto container_client = Azure::Storage::Blobs::BlobContainerClient(sas_uri, client_options);
    auto blob_client = container_client.GetBlobClient(blob_name);

    // Single shared Azure context for the whole blob; calling Cancel() on it
    // propagates into every in-flight chunk read.
    Azure::Core::Context azure_ctx;
    // Internal cancel flag flipped by the orchestrator on first chunk failure
    // or by external cancellation; checked by workers between iterations.
    std::atomic<bool> internal_cancel{false};

    ChunkContext chunk_ctx{blob_client, azure_ctx};

    int64_t blob_size = GetBlobSize(chunk_ctx);

    if (blob_size == 0) {
      EnsureEmptyBlobFile(local_path);
      BlobDownloadState::DeleteState(local_path, logger_);
      return;
    }

    // 2MB chunk size matching C#
    constexpr int64_t kChunkSize = 2 * 1024 * 1024;
    int32_t num_chunks = static_cast<int32_t>((blob_size + kChunkSize - 1) / kChunkSize);

    // Resume from existing sidecar if it matches the current blob layout.
    auto state = BlobDownloadState::LoadState(blob_name, local_path, blob_size,
                                              static_cast<int32_t>(kChunkSize),
                                              num_chunks, logger_);
    if (state) {
      // Only trust the sidecar if the data file it describes is actually on disk
      // at full size. If the data file was truncated or removed (e.g. an external
      // cleanup) while the sidecar survived, the chunks it marks complete are gone:
      // we would skip re-downloading them, Open() would recreate the file
      // zero-filled, and the result would be a silently corrupt file. Discard the
      // stale state and start fresh.
      std::error_code data_ec;
      auto data_size = std::filesystem::file_size(local_path, data_ec);
      if (data_ec || data_size != static_cast<uintmax_t>(blob_size)) {
        logger_.Log(LogLevel::Information,
                    "Resume sidecar for '" + local_path +
                        "' has no matching full-size data file; starting fresh");
        state.reset();
      }
    }

    if (!state) {
      state = BlobDownloadState::CreateNew(blob_name, local_path, blob_size,
                                           static_cast<int32_t>(kChunkSize), num_chunks);
      // Persist the sidecar now, before Open() pre-allocates the data file.
      // IsDownloadNeeded treats "data file at full size + no sidecar" as a
      // completed download and skips it. The periodic save below does not run
      // until save_interval chunks are done (~16 MB), so a crash between
      // pre-allocation and that first save would otherwise leave a full-size,
      // mostly-empty file with no sidecar that the next run silently accepts as
      // complete — serving zeros. Writing the sidecar up front upholds the
      // invariant "pre-allocated but unfinished <=> sidecar present" — so if it
      // can't be persisted we abort here, before Open() pre-allocates, rather
      // than risk a full-size file a later run reads as complete.
      if (!state->SaveState(logger_)) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
                 "failed to persist initial download state for '" + local_path + "'");
      }
    }

    // Track cumulative bytes for progress reporting; seed with bytes already
    // present on disk so percent stays monotonic across resume.
    std::atomic<int64_t> bytes_completed{state->CalculateDownloadedSize()};
    if (bytes_written_cb && bytes_completed.load() > 0) {
      bytes_written_cb(bytes_completed.load());
    }

    auto pending = state->GetPendingChunks();
    if (pending.empty()) {
      // Already complete on disk — drop the sidecar.
      BlobDownloadState::DeleteState(local_path, logger_);
      if (bytes_written_cb) {
        bytes_written_cb(blob_size);
      }
      return;
    }

    // Open the file writer once for the whole download. Open() pre-allocates
    // the file to blob_size if needed, preserving any existing bytes from a
    // resume. Concurrent WriteAt calls to disjoint ranges are thread-safe — the
    // OS arbitrates positional writes to non-overlapping ranges.
    FileWriter writer(logger_);
    writer.Open(local_path, blob_size);

    // Flush the resume sidecar roughly every 16 MB of completed chunks, so a
    // hard crash re-downloads at most that much on resume — a fixed bound,
    // independent of blob size. Checked only at chunk completion, so it never
    // flushes faster than chunks arrive.
    constexpr int64_t kBytesPerSidecarSave = 16 * 1024 * 1024;
    const int32_t save_interval =
        std::max(1, static_cast<int32_t>(kBytesPerSidecarSave / kChunkSize));
    std::atomic<int32_t> chunks_since_save{0};

    std::mutex error_mutex;
    std::exception_ptr first_error;

    // Worker pool: workers race to claim from `pending` via atomic fetch_add.
    // On any failure, the first worker to fail records the error, sets
    // internal_cancel, and calls azure_ctx.Cancel(); other workers see the
    // signal and exit fast.
    std::atomic<size_t> next_pending_idx{0};
    int worker_count = std::min<int>(max_concurrency, static_cast<int>(pending.size()));
    if (worker_count < 1) {
      worker_count = 1;
    }
    std::vector<std::future<void>> workers;
    workers.reserve(static_cast<size_t>(worker_count));

    auto worker_body = [&]() {
      // Per-worker scratch buffer reused across every chunk this worker
      // handles. Streaming downloads fill the scratch in 64 KB pieces and
      // forward each piece to the sink, so total transient memory is bounded
      // by `worker_count * kStreamingBufferBytes` regardless of chunk size.
      std::vector<uint8_t> scratch(kStreamingBufferBytes);

      while (true) {
        // External cancellation drains the pool as fast as the SDK can unwind.
        if (cancelled && cancelled->load(std::memory_order_relaxed)) {
          if (!internal_cancel.exchange(true)) {
            azure_ctx.Cancel();
          }
          return;
        }
        if (internal_cancel.load(std::memory_order_relaxed)) {
          return;
        }

        size_t i = next_pending_idx.fetch_add(1, std::memory_order_relaxed);
        if (i >= pending.size()) {
          return;
        }
        int32_t chunk_idx = pending[i];
        int64_t offset = static_cast<int64_t>(chunk_idx) * kChunkSize;
        int64_t size = std::min<int64_t>(kChunkSize, blob_size - offset);

        // Sink advances a per-chunk write cursor and forwards each piece to
        // the file writer. The writer is responsible for any synchronization
        // needed across concurrent workers; we don't take a mutex here.
        int64_t written = 0;
        auto sink = [&](const uint8_t* data, size_t len) {
          writer.WriteAt(offset + written, data, len);
          written += static_cast<int64_t>(len);
        };

        try {
          DownloadChunkStreaming(chunk_ctx, offset, size, scratch, sink);

          // Account for this chunk and fire the progress callback within the same
          // try as the download: on user cancellation bytes_written_cb throws, and
          // the catch below runs azure_ctx.Cancel() so peers blocked mid-chunk are
          // interrupted immediately rather than only noticing the cancel flag when
          // they finish their current chunk.
          // Report the global running total so progress stays monotonically
          // non-decreasing: concurrent workers complete chunks out of order, and
          // the public progress contract must never hand the callback a smaller
          // percentage after a larger one.
          bytes_completed.fetch_add(size, std::memory_order_relaxed);
          if (bytes_written_cb) {
            bytes_written_cb(bytes_completed.load(std::memory_order_relaxed));
          }
        } catch (...) {
          std::lock_guard<std::mutex> lock(error_mutex);
          if (!first_error) {
            first_error = std::current_exception();
          }
          if (!internal_cancel.exchange(true)) {
            azure_ctx.Cancel();
          }
          return;
        }

        bool should_save = false;
        {
          std::lock_guard<std::mutex> lock(state->mutex());
          state->MarkChunkComplete(chunk_idx);
          int32_t inc = chunks_since_save.fetch_add(1, std::memory_order_relaxed) + 1;
          // Skip the periodic save once every chunk is done: the finalization
          // path below deletes the sidecar on success, so writing a fully
          // complete sidecar here would just be undone microseconds later.
          if (inc >= save_interval && !state->IsComplete()) {
            chunks_since_save.store(0, std::memory_order_relaxed);
            should_save = true;
          }
        }
        if (should_save) {
          std::lock_guard<std::mutex> lock(state->mutex());
          state->SaveState(logger_);
        }
      }
    };

    for (int w = 0; w < worker_count; ++w) {
      workers.push_back(std::async(std::launch::async, worker_body));
    }

    for (auto& f : workers) {
      try {
        f.get();
      } catch (...) {
        // Worker bodies should already have routed exceptions through
        // first_error, but stay defensive in case std::async signals one.
        std::lock_guard<std::mutex> lock(error_mutex);
        if (!first_error) {
          first_error = std::current_exception();
        }
        internal_cancel.store(true, std::memory_order_relaxed);
      }
    }

    // Release the OS handle before persisting / deleting the sidecar so any
    // observer that watches the data file sees a fully-closed handle.
    writer.Close();

    const bool was_cancelled = cancelled && cancelled->load(std::memory_order_relaxed);
    if (first_error || was_cancelled) {
      // Persist what we have so the next attempt resumes from here.
      {
        std::lock_guard<std::mutex> lock(state->mutex());
        state->SaveState(logger_);
      }
      if (was_cancelled) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "download cancelled");
      }
      std::rethrow_exception(first_error);
    }

    // All chunks done — sidecar is no longer needed.
    BlobDownloadState::DeleteState(local_path, logger_);
  } catch (const Azure::Core::OperationCancelledException&) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "download cancelled");
  } catch (const Azure::Core::RequestFailedException& e) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_NETWORK,
             std::string("failed to download blob '") + blob_name + "': " + e.what());
  }
}

// ========================================================================
// DownloadBlobsToDirectory — high-level orchestration
// ========================================================================

namespace {

/// Compute relative destination path by stripping the prefix from blob name.
/// Matches C# GetItemDestinationPath_AzureFoundryImpl.
std::string ComputeRelativePath(const std::string& prefix, const std::string& blob_name) {
  if (prefix.empty()) {
    return blob_name;
  }

  if (blob_name.size() <= prefix.size()) {
    return blob_name;
  }

  auto trim = prefix.size();
  if (blob_name[trim] == '/') {
    ++trim;
  }

  return blob_name.substr(trim);
}

/// Returns false if a file at `local_path` already matches the blob's expected
/// `content_length` exactly AND has no `.dlstate` sidecar — in which case the
/// caller can skip the download. Returns true (download needed) for any of:
/// missing file, size mismatch, sidecar present (file may be pre-allocated
/// with holes), or filesystem-stat errors (treat as "redownload to be safe").
bool IsDownloadNeeded(const BlobItemInfo& blob, const std::string& local_path) {
  std::error_code ec;
  auto status = std::filesystem::status(local_path, ec);
  if (ec || !std::filesystem::exists(status) || !std::filesystem::is_regular_file(status)) {
    return true;
  }
  auto size = std::filesystem::file_size(local_path, ec);
  if (ec) {
    return true;
  }
  if (static_cast<int64_t>(size) != blob.content_length) {
    return true;
  }
  // The data file is at the expected size, but a sidecar means a previous run
  // pre-allocated then aborted mid-download. The file has holes; let
  // AzureBlobDownloader resume from the sidecar.
  auto sidecar = BlobDownloadState::GetStateFilePath(local_path);
  if (std::filesystem::exists(sidecar, ec)) {
    return true;
  }
  return false;
}

}  // anonymous namespace

void DownloadBlobsToDirectory(IBlobDownloader& downloader,
                              const std::string& sas_uri,
                              const std::string& output_directory,
                              const BlobDownloadOptions& options) {
  // Step 1: Enumerate all blobs
  auto all_blobs = downloader.ListBlobs(sas_uri);

  // Step 2: Filter by path prefix
  std::vector<std::pair<BlobItemInfo, std::string>> blobs_to_download;

  for (auto& blob : all_blobs) {
    if (!options.path_prefix.empty() &&
        blob.name.compare(0, options.path_prefix.size(), options.path_prefix) != 0) {
      continue;
    }

    auto relative_path = ComputeRelativePath(options.path_prefix, blob.name);

    // Normalize backslashes to forward slashes so path-traversal validation
    // is cross-platform: on POSIX, backslash is just a filename character and
    // would otherwise hide a `..\evil` segment from the canonicalization check.
    std::replace(relative_path.begin(), relative_path.end(), '\\', '/');

    auto local_path_fs = std::filesystem::path(output_directory) / relative_path;

    // Defense-in-depth: blob names come from a remote service. Reject any
    // blob whose computed destination would escape `output_directory` via
    // `..` segments or an absolute path. This check must happen BEFORE any
    // file create/open/write so a malicious entry cannot touch the disk.
    if (!IsPathWithinDirectory(local_path_fs, std::filesystem::path(output_directory))) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               std::string("blob path escapes destination directory: ") + blob.name);
    }

    auto local_path = local_path_fs.string();
    blobs_to_download.emplace_back(std::move(blob), std::move(local_path));
  }

  // Step 3: Filter out inference_model.json (matching C# AzureBlobDownloadClientProvider filter)
  blobs_to_download.erase(std::remove_if(blobs_to_download.begin(), blobs_to_download.end(),
                                         [](const auto& pair) {
                                           return EndsWithIgnoreCase(pair.first.name, "inference_model.json");
                                         }),
                          blobs_to_download.end());

  if (blobs_to_download.empty()) {
    return;
  }

  // Step 3.5: Sort smallest-first for faster initial perceived progress
  std::sort(blobs_to_download.begin(), blobs_to_download.end(),
            [](const auto& a, const auto& b) {
              return a.first.content_length < b.first.content_length;
            });

  // Step 4: Calculate total size across every in-scope blob, including those
  // already present on disk.
  int64_t total_size = 0;
  for (const auto& [blob, _] : blobs_to_download) {
    total_size += blob.content_length;
  }

  // Step 5: Skip blobs already present at the expected size. Their bytes
  // count toward "downloaded" so the percentage stays accurate when this is a
  // resume of a partially-completed download.
  int64_t skipped_bytes = 0;
  blobs_to_download.erase(
      std::remove_if(blobs_to_download.begin(), blobs_to_download.end(),
                     [&skipped_bytes](const auto& pair) {
                       if (IsDownloadNeeded(pair.first, pair.second)) {
                         return false;
                       }
                       skipped_bytes += pair.first.content_length;
                       return true;
                     }),
      blobs_to_download.end());

  // Step 6: Emit initial progress reflecting any already-on-disk bytes.
  // If everything was skipped, emit 100% directly and return.
  if (blobs_to_download.empty()) {
    if (options.progress) {
      options.progress(100.0f);
    }
    return;
  }

  if (options.progress) {
    float initial_percent =
        total_size > 0 ? static_cast<float>(skipped_bytes) / static_cast<float>(total_size) * 100.0f : 0.0f;
    int result = options.progress(initial_percent);
    if (result != 0) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "download cancelled by user callback return value");
    }
  }

  // Step 7: Download each blob with per-chunk progress.
  // The cancellation flag is set when the progress callback returns non-zero.
  // It is shared with chunk download threads so they can exit promptly.
  std::atomic<bool> cancelled{false};
  // Seed with skipped bytes so per-chunk progress callbacks compute the right
  // overall percentage.
  std::atomic<int64_t> total_downloaded_bytes{skipped_bytes};

  // The user progress callback can be reached from up to max_concurrency chunk
  // worker threads at once (per_chunk_progress below). Serialize it so a
  // caller's callback (UI handle, counter, logger, IPC) is never entered
  // concurrently — the public download progress API does not require callers to
  // be thread-safe.
  std::mutex progress_mutex;

  for (const auto& [blob, local_path] : blobs_to_download) {
    // Check cancellation between blobs
    if (cancelled.load(std::memory_order_relaxed)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "download cancelled by user callback return value");
    }

    // Ensure parent directory exists
    auto parent = std::filesystem::path(local_path).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    // Capture the byte count at the start of this blob so per-chunk callbacks
    // can compute total progress across all blobs.
    int64_t blob_base_bytes = total_downloaded_bytes.load();

    BlobBytesWrittenFn per_chunk_progress;
    if (options.progress && total_size > 0) {
      per_chunk_progress = [&](int64_t blob_bytes_so_far) {
        int64_t overall = blob_base_bytes + blob_bytes_so_far;
        // Don't exceed total_size (may happen if blob sizes changed between list and download)
        overall = std::min(overall, total_size);

        float percent = static_cast<float>(overall) / static_cast<float>(total_size) * 100.0f;
        int result;
        {
          std::lock_guard<std::mutex> lock(progress_mutex);
          result = options.progress(percent);
        }
        if (result != 0) {
          cancelled.store(true, std::memory_order_relaxed);
          FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "download cancelled by user callback return value");
        }
      };
    }

    downloader.DownloadBlob(sas_uri, blob.name, local_path, options.max_concurrency,
                            per_chunk_progress, &cancelled);

    total_downloaded_bytes += blob.content_length;
  }

  // Final progress
  if (options.progress) {
    options.progress(100.0f);
  }
}

}  // namespace fl
