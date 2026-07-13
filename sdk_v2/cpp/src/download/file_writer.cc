// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "download/file_writer.h"
#include "exception.h"
#include "logger.h"
#include "platform/file_io.h"

#include <foundry_local/foundry_local_c.h>

#include <fstream>
#include <string>
#include <system_error>

namespace fl {

namespace fs = std::filesystem;

namespace {

/// Ensure the data file exists at exactly `expected_size`, recreating it at the
/// new size if it currently differs (larger or smaller). An existing file that
/// is already the right size is left intact — the resume path relies on this.
void EnsureFileExistsAtSize(const fs::path& path, int64_t expected_size) {
  std::error_code ec;
  auto cur_size = fs::file_size(path, ec);
  if (!ec) {
    if (cur_size == static_cast<uintmax_t>(expected_size)) {
      return;
    }
    // File exists but is the wrong size — fall through to recreate.
  } else if (ec != std::errc::no_such_file_or_directory) {
    // Some other stat error (permission, transient NFS hiccup, AV scanner
    // holding a handle, etc.). Don't blow away a potentially-intact file just
    // because we couldn't read its size; surface the error instead so the
    // caller can retry and the existing on-disk progress is preserved.
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "failed to stat blob file: " + path.string() + " (" + ec.message() + ")");
  }

  std::ofstream f(path, std::ios::binary);
  if (!f.is_open()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "failed to open blob file for pre-allocation: " + path.string());
  }
  if (expected_size > 0) {
    f.seekp(expected_size - 1);
    f.put('\0');
  }
  f.close();
  if (f.fail()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "failed to pre-allocate blob file: " + path.string() +
                 " (size=" + std::to_string(expected_size) + ")");
  }
}

}  // namespace

FileWriter::FileWriter(ILogger& logger) : logger_(logger) {}

FileWriter::~FileWriter() { Close(); }

void FileWriter::Open(const fs::path& path, int64_t expected_size) {
  EnsureFileExistsAtSize(path, expected_size);
  std::string error;
  handle_ = platform::OpenWritableFile(path, error);
  if (handle_ == platform::kInvalidFileHandle) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "FileWriter open failed for " + path.string() + " (" + error + ")");
  }
}

void FileWriter::WriteAt(int64_t offset, const uint8_t* data, size_t len) {
  std::string error;
  if (!platform::WriteFileAt(handle_, offset, data, len, error)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "FileWriter " + error);
  }
}

void FileWriter::Close() {
  if (handle_ != platform::kInvalidFileHandle) {
    std::string error;
    if (!platform::CloseFile(handle_, error)) {
      logger_.Log(LogLevel::Warning, "FileWriter: " + error);
    }
    handle_ = platform::kInvalidFileHandle;
  }
}

}  // namespace fl
