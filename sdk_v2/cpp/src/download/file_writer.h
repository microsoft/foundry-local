// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace fl {

class ILogger;

/// Thread-safe positional writer for blob downloads.
///
/// Workers in a single download claim disjoint chunks, so concurrent `WriteAt`
/// calls always target non-overlapping byte ranges. Backed by `pwrite` (POSIX)
/// or `WriteFile` + `OVERLAPPED` (Windows): the OS arbitrates concurrent writes
/// to disjoint ranges, so no user-space lock is taken.
class FileWriter {
 public:
  explicit FileWriter(ILogger& logger);
  ~FileWriter();

  FileWriter(const FileWriter&) = delete;
  FileWriter& operator=(const FileWriter&) = delete;

  /// Make `path` exist at exactly `expected_size` bytes. If the file already
  /// exists at that size, leave its contents intact so the resume path can pick
  /// up where it left off. Called once before the first `WriteAt`.
  void Open(const std::filesystem::path& path, int64_t expected_size);

  /// Write `len` bytes from `data` starting at byte offset `offset`. Safe for
  /// concurrent calls targeting disjoint ranges.
  void WriteAt(int64_t offset, const uint8_t* data, size_t len);

  /// Release the underlying OS handle. Implicitly called by the destructor.
  void Close();

 private:
  ILogger& logger_;
#ifdef _WIN32
  // Win32 HANDLE. Holds a valid handle while open, nullptr otherwise.
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

}  // namespace fl
