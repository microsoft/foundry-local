// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "download/file_writer.h"
#include "exception.h"
#include "logger.h"

#include <foundry_local/foundry_local_c.h>

#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#endif

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

#ifdef _WIN32

FileWriter::~FileWriter() { Close(); }

void FileWriter::Open(const fs::path& path, int64_t expected_size) {
  EnsureFileExistsAtSize(path, expected_size);
  // FILE_SHARE_READ | FILE_SHARE_WRITE so the lock file / other tools can peek
  // at the partial file without us erroring; positional WriteFile is safe
  // regardless of share mode.
  HANDLE h = ::CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "FileWriter open failed for " + path.string() + " (Win32 err " +
                 std::to_string(::GetLastError()) + ")");
  }
  handle_ = h;
}

void FileWriter::WriteAt(int64_t offset, const uint8_t* data, size_t len) {
  // Concurrent WriteFile calls with distinct OVERLAPPED offsets on the same
  // handle are safe for non-overlapping ranges; the kernel orders them.
  while (len > 0) {
    OVERLAPPED ov{};
    // Split the 64-bit file offset across the OVERLAPPED halves: the DWORD casts
    // keep the low 32 bits in Offset and the high 32 bits in OffsetHigh.
    ov.Offset = static_cast<DWORD>(static_cast<uint64_t>(offset));
    ov.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);
    DWORD to_write = static_cast<DWORD>(len > 0x7FFFFFFFu ? 0x7FFFFFFFu : len);
    DWORD written = 0;
    if (!::WriteFile(handle_, data, to_write, &written, &ov)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "FileWriter write failed at offset " + std::to_string(offset) + " (Win32 err " +
                   std::to_string(::GetLastError()) + ")");
    }
    if (written == 0) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "FileWriter short write at offset " + std::to_string(offset));
    }
    offset += static_cast<int64_t>(written);
    data += written;
    len -= written;
  }
}

void FileWriter::Close() {
  if (handle_ != nullptr) {
    if (!::CloseHandle(handle_)) {
      const DWORD err = ::GetLastError();
      logger_.Log(LogLevel::Warning,
                  "FileWriter: CloseHandle failed (Win32 err " + std::to_string(err) + ")");
    }
    handle_ = nullptr;
  }
}

#else  // POSIX

FileWriter::~FileWriter() { Close(); }

void FileWriter::Open(const fs::path& path, int64_t expected_size) {
  EnsureFileExistsAtSize(path, expected_size);
  fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd_ < 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
             "FileWriter open failed for " + path.string() + " (errno " +
                 std::to_string(errno) + ")");
  }
}

void FileWriter::WriteAt(int64_t offset, const uint8_t* data, size_t len) {
  while (len > 0) {
    ssize_t n = ::pwrite(fd_, data, len, static_cast<off_t>(offset));
    if (n < 0) {
      if (errno == EINTR) continue;
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "FileWriter pwrite failed at offset " + std::to_string(offset) + " (errno " +
                   std::to_string(errno) + ")");
    }
    if (n == 0) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "FileWriter short pwrite at offset " + std::to_string(offset));
    }
    offset += n;
    data += n;
    len -= static_cast<size_t>(n);
  }
}

void FileWriter::Close() {
  if (fd_ >= 0) {
    // A failing close() can surface a deferred write error (e.g. EIO, or ENOSPC
    // on delayed allocation / a networked filesystem), so the file may be
    // incomplete even though every pwrite returned success. Log it for
    // diagnosis. Don't retry: on Linux the descriptor is freed even when close()
    // returns EINTR, so a retry could close an unrelated, since-reused fd.
    if (::close(fd_) != 0) {
      const int err = errno;
      logger_.Log(LogLevel::Warning,
                  "FileWriter: close failed (errno " + std::to_string(err) + ")");
    }
    fd_ = -1;
  }
}

#endif

}  // namespace fl
