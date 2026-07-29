// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "platform/file_io.h"

#include <cerrno>
#include <fcntl.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>

namespace fl::platform {

std::intptr_t OpenWritableFile(const std::filesystem::path& path, std::string& error) {
  int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    error = "errno " + std::to_string(errno);
    return kInvalidFileHandle;
  }
  return static_cast<std::intptr_t>(fd);
}

bool WriteFileAt(std::intptr_t handle, std::int64_t offset, const std::uint8_t* data, std::size_t len,
                 std::string& error) {
  int fd = static_cast<int>(handle);
  while (len > 0) {
    ssize_t n = ::pwrite(fd, data, len, static_cast<off_t>(offset));
    if (n < 0) {
      if (errno == EINTR) continue;
      error = "pwrite failed at offset " + std::to_string(offset) + " (errno " +
              std::to_string(errno) + ")";
      return false;
    }
    if (n == 0) {
      error = "short pwrite at offset " + std::to_string(offset);
      return false;
    }
    offset += n;
    data += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool CloseFile(std::intptr_t handle, std::string& error) {
  int fd = static_cast<int>(handle);
  // A failing close() can surface a deferred write error (e.g. EIO, or ENOSPC on
  // delayed allocation / a networked filesystem), so the file may be incomplete
  // even though every pwrite returned success. Don't retry: on Linux the
  // descriptor is freed even when close() returns EINTR, so a retry could close
  // an unrelated, since-reused fd.
  if (::close(fd) != 0) {
    error = "close failed (errno " + std::to_string(errno) + ")";
    return false;
  }
  return true;
}

}  // namespace fl::platform
