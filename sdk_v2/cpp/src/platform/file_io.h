// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace fl::platform {

/// Native file handle (a Win32 HANDLE or a POSIX file descriptor) carried as an
/// integer so callers don't depend on platform headers. `kInvalidFileHandle`
/// matches both INVALID_HANDLE_VALUE ((HANDLE)-1) and a POSIX fd of -1.
inline constexpr std::intptr_t kInvalidFileHandle = -1;

/// Open an existing file for positional read/write. Returns a handle that is not
/// `kInvalidFileHandle` on success; on failure returns `kInvalidFileHandle` and
/// sets `error` to a human-readable OS error (e.g. "Win32 err 5" / "errno 13").
std::intptr_t OpenWritableFile(const std::filesystem::path& path, std::string& error);

/// Write all `len` bytes from `data` at byte offset `offset`. Safe for concurrent
/// calls on the same handle targeting disjoint ranges. Returns true on success;
/// on failure returns false and sets `error` (e.g. "pwrite failed at offset N
/// (errno M)").
bool WriteFileAt(std::intptr_t handle, std::int64_t offset, const std::uint8_t* data, std::size_t len,
                 std::string& error);

/// Close `handle`. Returns true on success; on failure returns false and sets
/// `error`. A failing close can surface a deferred write error, so the data may
/// be incomplete even when every write succeeded.
bool CloseFile(std::intptr_t handle, std::string& error);

}  // namespace fl::platform
