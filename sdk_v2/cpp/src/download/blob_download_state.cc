// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "download/blob_download_state.h"
#include "logger.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <system_error>
#include <type_traits>

namespace fl {

namespace {

constexpr const char* kStateFileExtension = ".dlstate";

// On-disk format. Scalar fields use host byte order (little-endian on every
// target we build for); see WriteNative/ReadNative below. The bitmap suffix is
// a raw byte copy and is endian-agnostic.
//   bytes  | field
//   -------|--------------------------------------------------------
//   0..3   | magic "FLDS"
//   4      | version (currently 1)
//   5..12  | blob_size                   (int64)
//   13..16 | chunk_size                  (int32)
//   17..20 | total_chunks                (int32)
//   21..24 | bitmap_byte_aligned_start   (int32)
//   25..28 | highest_completed_chunk     (int32)
//   29..32 | completed_count             (int32)
//   33..40 | last_modified_unix_ms       (int64)
//   41..44 | trunc_bitmap_byte_len       (uint32)
//   45..   | trunc_bitmap_byte_len bytes of bitmap data, copied directly out of
//           full_completion_bitmap starting at the byte offset implied by
//           bitmap_byte_aligned_start.
constexpr char kMagic[4] = {'F', 'L', 'D', 'S'};
constexpr uint8_t kVersion = 1;

constexpr int32_t kBitsPerWord = 64;

// Serialize a scalar field in host byte order. Every target we build for
// (x64 / arm64) is little-endian, so the on-disk layout is little-endian in
// practice.
template <typename T>
void WriteNative(std::ostream& out, T value) {
  static_assert(std::is_trivially_copyable_v<T>);
  out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool ReadNative(std::istream& in, T& out_value) {
  static_assert(std::is_trivially_copyable_v<T>);
  in.read(reinterpret_cast<char*>(&out_value), sizeof(T));
  return static_cast<bool>(in);
}

int64_t NowUnixMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

std::filesystem::path BlobDownloadState::GetStateFilePath(const std::filesystem::path& local_file_path) {
  auto p = local_file_path;
  p += kStateFileExtension;
  return p;
}

std::unique_ptr<BlobDownloadState> BlobDownloadState::CreateNew(std::string blob_name,
                                                                const std::filesystem::path& local_file_path,
                                                                int64_t blob_size,
                                                                int32_t chunk_size,
                                                                int32_t total_chunks) {
  auto state = std::make_unique<BlobDownloadState>();
  state->blob_name = std::move(blob_name);
  state->local_file_path = local_file_path.string();
  state->blob_size = blob_size;
  state->chunk_size = chunk_size;
  state->total_chunks = total_chunks;
  state->bitmap_byte_aligned_start = 0;
  state->highest_completed_chunk = -1;
  state->completed_count = 0;
  state->last_modified_unix_ms = NowUnixMs();
  auto words = static_cast<size_t>((total_chunks + kBitsPerWord - 1) / kBitsPerWord);
  state->full_completion_bitmap.assign(words, 0);
  return state;
}

std::unique_ptr<BlobDownloadState> BlobDownloadState::LoadState(std::string blob_name,
                                                                const std::filesystem::path& local_file_path,
                                                                int64_t expected_blob_size,
                                                                int32_t expected_chunk_size,
                                                                int32_t expected_total_chunks,
                                                                ILogger& logger) {
  auto state_path = GetStateFilePath(local_file_path);
  std::error_code ec;
  if (!std::filesystem::exists(state_path, ec)) {
    return nullptr;
  }

  std::ifstream in(state_path, std::ios::binary);
  if (!in) {
    logger.Log(LogLevel::Warning, "Could not open download state file: " + state_path.string());
    return nullptr;
  }

  char magic[4]{};
  in.read(magic, 4);
  uint8_t version = 0;
  if (!in || std::memcmp(magic, kMagic, 4) != 0 || !ReadNative(in, version) || version != kVersion) {
    logger.Log(LogLevel::Warning,
               "Download state file " + state_path.string() + " has unexpected magic/version; ignoring");
    return nullptr;
  }

  int64_t blob_size = 0;
  int32_t chunk_size = 0;
  int32_t total_chunks = 0;
  int32_t bitmap_byte_aligned_start = 0;
  int32_t highest_completed_chunk = 0;
  int32_t completed_count = 0;
  int64_t last_modified_unix_ms = 0;
  uint32_t trunc_len = 0;
  if (!ReadNative(in, blob_size) || !ReadNative(in, chunk_size) || !ReadNative(in, total_chunks) ||
      !ReadNative(in, bitmap_byte_aligned_start) || !ReadNative(in, highest_completed_chunk) ||
      !ReadNative(in, completed_count) || !ReadNative(in, last_modified_unix_ms) || !ReadNative(in, trunc_len)) {
    logger.Log(LogLevel::Warning, "Download state header truncated: " + state_path.string());
    return nullptr;
  }

  // Sanity / compatibility checks.
  if (blob_size != expected_blob_size || chunk_size != expected_chunk_size ||
      total_chunks != expected_total_chunks) {
    logger.Log(LogLevel::Information,
               "Download state for " + state_path.string() +
                   " is incompatible with current blob layout; starting fresh");
    return nullptr;
  }
  if (bitmap_byte_aligned_start < 0 || bitmap_byte_aligned_start % 8 != 0 ||
      bitmap_byte_aligned_start > total_chunks || completed_count < 0 ||
      completed_count > total_chunks || highest_completed_chunk < -1 ||
      highest_completed_chunk >= total_chunks) {
    logger.Log(LogLevel::Warning, "Download state header values out of range: " + state_path.string());
    return nullptr;
  }

  auto words_total = static_cast<size_t>((total_chunks + kBitsPerWord - 1) / kBitsPerWord);
  std::vector<uint64_t> bitmap(words_total, 0);

  // The prefix of fully-completed chunks below bitmap_byte_aligned_start is
  // implied — fill those bits.
  size_t implicit_full_words = static_cast<size_t>(bitmap_byte_aligned_start) / kBitsPerWord;
  for (size_t i = 0; i < implicit_full_words && i < bitmap.size(); ++i) {
    bitmap[i] = ~uint64_t{0};
  }
  // Any remaining "implicit" bits inside a partial word (between
  // implicit_full_words*64 and bitmap_byte_aligned_start).
  if (size_t partial_bits = static_cast<size_t>(bitmap_byte_aligned_start) % kBitsPerWord;
      partial_bits > 0 && implicit_full_words < bitmap.size()) {
    bitmap[implicit_full_words] |= (uint64_t{1} << partial_bits) - 1;
  }

  if (trunc_len > 0) {
    // Copy serialized bytes directly into the bitmap starting at the byte
    // position implied by bitmap_byte_aligned_start.
    size_t byte_offset = static_cast<size_t>(bitmap_byte_aligned_start) / 8;
    auto* dest = reinterpret_cast<unsigned char*>(bitmap.data()) + byte_offset;
    auto dest_capacity = bitmap.size() * sizeof(uint64_t) - byte_offset;
    if (trunc_len > dest_capacity) {
      logger.Log(LogLevel::Warning,
                 "Download state bitmap length exceeds expected capacity: " + state_path.string());
      return nullptr;
    }
    in.read(reinterpret_cast<char*>(dest), trunc_len);
    if (!in) {
      logger.Log(LogLevel::Warning,
                 "Download state bitmap payload truncated: " + state_path.string());
      return nullptr;
    }
  }

  auto state = std::make_unique<BlobDownloadState>();
  state->blob_name = std::move(blob_name);
  state->local_file_path = local_file_path.string();
  state->blob_size = blob_size;
  state->chunk_size = chunk_size;
  state->total_chunks = total_chunks;
  state->bitmap_byte_aligned_start = bitmap_byte_aligned_start;
  state->highest_completed_chunk = highest_completed_chunk;
  state->completed_count = completed_count;
  state->last_modified_unix_ms = last_modified_unix_ms;
  state->full_completion_bitmap = std::move(bitmap);

  logger.Log(LogLevel::Information,
             "Loaded download state " + state_path.string() + ": " +
                 std::to_string(completed_count) + "/" + std::to_string(total_chunks) +
                 " chunks already done");
  return state;
}

int64_t BlobDownloadState::CalculateDownloadedSize() const noexcept {
  int64_t bytes = static_cast<int64_t>(completed_count) * chunk_size;
  // If the final chunk is partial and was completed, adjust the overcount.
  if (highest_completed_chunk == total_chunks - 1 && chunk_size > 0) {
    auto remainder = blob_size % chunk_size;
    if (remainder != 0) {
      bytes -= (chunk_size - remainder);
    }
  }
  return bytes;
}

bool BlobDownloadState::IsChunkComplete(int32_t chunk_idx) const noexcept {
  if (chunk_idx < 0 || chunk_idx >= total_chunks) {
    return false;
  }
  if (chunk_idx < bitmap_byte_aligned_start) {
    // Below the truncation point — implicitly complete.
    return true;
  }
  auto word_idx = static_cast<size_t>(chunk_idx) / kBitsPerWord;
  auto bit_idx = static_cast<size_t>(chunk_idx) % kBitsPerWord;
  if (word_idx >= full_completion_bitmap.size()) {
    return false;
  }
  return (full_completion_bitmap[word_idx] & (uint64_t{1} << bit_idx)) != 0;
}

void BlobDownloadState::MarkChunkComplete(int32_t chunk_idx) {
  if (chunk_idx < 0 || chunk_idx >= total_chunks) {
    return;
  }
  if (IsChunkComplete(chunk_idx)) {
    return;
  }
  if (chunk_idx > highest_completed_chunk) {
    highest_completed_chunk = chunk_idx;
  }
  auto word_idx = static_cast<size_t>(chunk_idx) / kBitsPerWord;
  auto bit_idx = static_cast<size_t>(chunk_idx) % kBitsPerWord;
  full_completion_bitmap[word_idx] |= (uint64_t{1} << bit_idx);
  ++completed_count;
}

std::vector<int32_t> BlobDownloadState::GetPendingChunks() const {
  std::vector<int32_t> pending;
  pending.reserve(static_cast<size_t>(total_chunks - completed_count));
  for (int32_t i = bitmap_byte_aligned_start; i < total_chunks; ++i) {
    if (!IsChunkComplete(i)) {
      pending.push_back(i);
    }
  }
  return pending;
}

bool BlobDownloadState::SaveState(ILogger& logger) {
  // Advance bitmap_byte_aligned_start past any words that are now all 1s, so
  // the next save serializes only the unfinished tail.
  // Find the first word that is not fully complete. Every word below it is
  // implicitly complete and need not be serialized again.
  size_t word_idx = static_cast<size_t>(bitmap_byte_aligned_start) / kBitsPerWord;
  while (word_idx < full_completion_bitmap.size() &&
         full_completion_bitmap[word_idx] == ~uint64_t{0}) {
    ++word_idx;
  }
  int32_t new_start;
  if (word_idx < full_completion_bitmap.size()) {
    // Within the first not-fully-set word, advance to the lowest 0 bit. Derive
    // the absolute chunk index from the word base (word_idx * 64), NOT by
    // accumulating 64 per word onto the (possibly unaligned) previous start —
    // the latter overshoots by (bitmap_byte_aligned_start % 64) and would mark
    // never-downloaded chunks complete on reload. Round down to a byte boundary
    // so reload-then-resume re-reads on a clean alignment.
    uint64_t inverted = ~full_completion_bitmap[word_idx];
    int trailing_zero = 0;
    while (trailing_zero < kBitsPerWord && ((inverted >> trailing_zero) & 1) == 0) {
      ++trailing_zero;
    }
    new_start = static_cast<int32_t>(word_idx) * kBitsPerWord + trailing_zero;
  } else {
    // Every word is fully complete.
    new_start = total_chunks;
  }
  new_start = (new_start / 8) * 8;
  if (new_start > total_chunks) {
    new_start = (total_chunks / 8) * 8;
  }
  if (new_start > bitmap_byte_aligned_start) {
    bitmap_byte_aligned_start = new_start;
  }

  last_modified_unix_ms = NowUnixMs();

  auto state_path = GetStateFilePath(local_file_path);
  auto tmp_path = state_path;
  tmp_path += ".tmp";

  // Compute the serialized bitmap payload: bytes from bitmap_byte_aligned_start
  // up to (highest_completed_chunk + 1), rounded up to the nearest byte.
  uint32_t trunc_len = 0;
  if (highest_completed_chunk >= bitmap_byte_aligned_start) {
    int32_t bit_count = highest_completed_chunk - bitmap_byte_aligned_start + 1;
    trunc_len = static_cast<uint32_t>((bit_count + 7) / 8);
  }
  size_t byte_offset = static_cast<size_t>(bitmap_byte_aligned_start) / 8;

  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      logger.Log(LogLevel::Error, "Failed to open download state tmp file: " + tmp_path.string());
      return false;
    }
    out.write(kMagic, 4);
    WriteNative(out, kVersion);
    WriteNative(out, blob_size);
    WriteNative(out, chunk_size);
    WriteNative(out, total_chunks);
    WriteNative(out, bitmap_byte_aligned_start);
    WriteNative(out, highest_completed_chunk);
    WriteNative(out, completed_count);
    WriteNative(out, last_modified_unix_ms);
    WriteNative(out, trunc_len);
    if (trunc_len > 0) {
      auto* src = reinterpret_cast<const unsigned char*>(full_completion_bitmap.data()) + byte_offset;
      out.write(reinterpret_cast<const char*>(src), trunc_len);
    }
    if (!out) {
      logger.Log(LogLevel::Error, "Failed to write download state tmp file: " + tmp_path.string());
      return false;
    }
  }

  std::error_code ec;
  std::filesystem::rename(tmp_path, state_path, ec);
  if (ec) {
    // std::filesystem::rename atomically replaces the destination on every
    // platform we target (POSIX rename(2); Windows MoveFileExW with
    // MOVEFILE_REPLACE_EXISTING). If it still fails, the cause is transient
    // (e.g. a brief sharing violation on Windows or a flaky network FS) —
    // do NOT delete state_path as a fallback; that loses the only intact
    // copy of the resume bitmap. Instead, drop the tmp file and let the
    // next SaveState call retry from the up-to-date in-memory state.
    std::error_code rm_ec;
    std::filesystem::remove(tmp_path, rm_ec);
    logger.Log(LogLevel::Error,
               "Failed to commit download state file: " + tmp_path.string() + " -> " +
                   state_path.string() + " (" + ec.message() +
                   "); previous state retained, will retry on next save");
    return false;
  }
  return true;
}

void BlobDownloadState::DeleteState(const std::filesystem::path& local_file_path, ILogger& logger) {
  auto state_path = GetStateFilePath(local_file_path);
  std::error_code ec;
  std::filesystem::remove(state_path, ec);
  if (ec) {
    logger.Log(LogLevel::Warning,
               "Failed to delete download state file: " + state_path.string() + " (" +
                   ec.message() + ")");
  }
}

}  // namespace fl
