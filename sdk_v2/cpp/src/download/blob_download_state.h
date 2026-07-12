// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fl {

class ILogger;

/// Per-blob download progress, persisted next to the data file as `<file>.dlstate`.
///
/// Each chunk completion flips a bit in `full_completion_bitmap`. On resume,
/// `GetPendingChunks` enumerates only chunks whose bits are still 0.
///
/// The serialized form stores only the bitmap suffix starting at
/// `bitmap_byte_aligned_start` to `highest_completed_chunk`.
/// This keeps the on-disk state proportional to the *unfinished*
/// range, not the total file size.
///
/// On-disk layout is a small fixed-width little-endian binary header followed
/// by the truncated bitmap bytes.
class BlobDownloadState {
 public:
  /// Identity of the blob (populated by caller; not serialized).
  std::string blob_name;
  std::string local_file_path;

  /// Fixed at first save; serialized for resume integrity checks.
  int64_t blob_size = 0;
  int32_t chunk_size = 0;
  int32_t total_chunks = 0;

  /// Serialization marker (always a multiple of 8): chunks below this index are
  /// complete and dropped from the sidecar's truncated bitmap. The in-memory
  /// `full_completion_bitmap` still covers them.
  int32_t bitmap_byte_aligned_start = 0;

  /// Highest chunk index completed so far. -1 if no chunks are done yet.
  int32_t highest_completed_chunk = -1;

  /// Cached count for O(1) `IsComplete()`.
  int32_t completed_count = 0;

  /// Unix epoch milliseconds; refreshed on every save.
  int64_t last_modified_unix_ms = 0;

  /// One bit per chunk over the whole blob: chunk `i` lives in word `i / 64` at
  /// bit `i % 64` (absolute indexing — the buffer always starts at chunk 0).
  /// Sized for all `total_chunks` by `CreateNew`; `MarkChunkComplete` sets bits
  /// without resizing.
  std::vector<uint64_t> full_completion_bitmap;

  /// Sidecar path for `local_file_path`.
  static std::filesystem::path GetStateFilePath(const std::filesystem::path& local_file_path);

  /// Construct a fresh state for a new download. Bitmap sized for `total_chunks`.
  static std::unique_ptr<BlobDownloadState> CreateNew(std::string blob_name,
                                                      const std::filesystem::path& local_file_path,
                                                      int64_t blob_size,
                                                      int32_t chunk_size,
                                                      int32_t total_chunks);

  /// Load existing state from `<local_file_path>.dlstate`. Returns nullptr if
  /// the file does not exist, is corrupted, or has incompatible
  /// `blob_size` / `chunk_size` / `total_chunks` (caller-provided values are
  /// authoritative — a mismatch means the blob has been reconfigured upstream
  /// and the partial download is no longer valid).
  /// `logger` receives diagnostics for corrupt/incompatible state files. Required: the
  /// downloader always has a logger, so there is no optional/null case to handle.
  static std::unique_ptr<BlobDownloadState> LoadState(std::string blob_name,
                                                      const std::filesystem::path& local_file_path,
                                                      int64_t expected_blob_size,
                                                      int32_t expected_chunk_size,
                                                      int32_t expected_total_chunks,
                                                      ILogger& logger);

  /// All chunks downloaded.
  bool IsComplete() const noexcept { return completed_count == total_chunks; }

  /// Sum of bytes already written. Accounts for the final chunk being smaller
  /// than `chunk_size` when blob_size is not chunk-aligned.
  int64_t CalculateDownloadedSize() const noexcept;

  /// Whether `chunk_idx` is already marked complete.
  bool IsChunkComplete(int32_t chunk_idx) const noexcept;

  /// Mark `chunk_idx` complete. Caller must hold the mutex when called from
  /// concurrent worker tasks (use `mutex()` for that). Idempotent.
  void MarkChunkComplete(int32_t chunk_idx);

  /// Enumerate chunks in [0, total_chunks) that are not yet complete.
  std::vector<int32_t> GetPendingChunks() const;

  /// Atomically write current state to `<local_file_path>.dlstate`. Returns true
  /// on success; on failure it logs and returns false rather than throwing. Most
  /// callers treat a failed periodic save as best-effort (the next save retries,
  /// and resume just replays a few chunks); the initial pre-allocation save
  /// treats false as fatal, since the "pre-allocated <=> sidecar present"
  /// invariant depends on it. `logger` is required.
  bool SaveState(ILogger& logger);

  /// Remove the sidecar; called on successful completion.
  static void DeleteState(const std::filesystem::path& local_file_path,
                          ILogger& logger);

  /// Mutex protecting concurrent `MarkChunkComplete` / `SaveState` calls from
  /// the chunk worker pool.
  std::mutex& mutex() noexcept { return mutex_; }

 private:
  mutable std::mutex mutex_;
};

}  // namespace fl
