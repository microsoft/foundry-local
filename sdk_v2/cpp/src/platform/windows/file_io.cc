// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "platform/file_io.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace fl::platform {

std::intptr_t OpenWritableFile(const std::filesystem::path& path, std::string& error) {
  // FILE_SHARE_READ | FILE_SHARE_WRITE so the lock file / other tools can peek
  // at the partial file without us erroring; positional WriteFile is safe
  // regardless of share mode.
  HANDLE h = ::CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    error = "Win32 err " + std::to_string(::GetLastError());
    return kInvalidFileHandle;
  }
  return reinterpret_cast<std::intptr_t>(h);
}

bool WriteFileAt(std::intptr_t handle, std::int64_t offset, const std::uint8_t* data, std::size_t len,
                 std::string& error) {
  HANDLE h = reinterpret_cast<HANDLE>(handle);
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
    if (!::WriteFile(h, data, to_write, &written, &ov)) {
      error = "write failed at offset " + std::to_string(offset) + " (Win32 err " +
              std::to_string(::GetLastError()) + ")";
      return false;
    }
    if (written == 0) {
      error = "short write at offset " + std::to_string(offset);
      return false;
    }
    offset += static_cast<int64_t>(written);
    data += written;
    len -= written;
  }
  return true;
}

bool CloseFile(std::intptr_t handle, std::string& error) {
  HANDLE h = reinterpret_cast<HANDLE>(handle);
  if (!::CloseHandle(h)) {
    error = "CloseHandle failed (Win32 err " + std::to_string(::GetLastError()) + ")";
    return false;
  }
  return true;
}

}  // namespace fl::platform
