// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace fl {

class ILogger;

/// Returns true if `entry` is a safe archive entry path, meaning extracting it
/// will not escape the destination directory ("zip-slip" defense). Rejects:
///   * any entry with a `..` path component (split on '/' or '\\')
///   * absolute POSIX paths (starting with '/')
///   * Windows absolute paths (drive-letter `X:` prefix or leading '\\')
bool IsSafeArchiveEntry(std::string_view entry);

/// Resource bounds enforced by ExtractZip to defend against zip-bomb / resource-exhaustion
/// archives. Defaults are generous for legitimate EP packages (hundreds of small files,
/// each well under a gigabyte) while still rejecting pathological inputs.
struct ZipExtractLimits {
  size_t max_entries = 20000;
  uint64_t max_total_uncompressed_bytes = 8ULL * 1024 * 1024 * 1024;  // 8 GiB
  uint64_t max_entry_uncompressed_bytes = 4ULL * 1024 * 1024 * 1024;
};

/// Extract a ZIP archive to a directory using an in-process parser (no subprocess, no shell).
/// Creates the destination directory if it doesn't exist.
///
/// Every central-directory entry is validated before any bytes are written:
///   * zip-slip defense (see IsSafeArchiveEntry)
///   * symlinks and special files (character/block devices, FIFOs, sockets) are rejected
///   * duplicate entry paths are rejected
///   * entry count and per-entry / total uncompressed size are bounded by `limits`
///   * only the STORE and DEFLATE compression methods are supported
/// Extraction only begins once every entry in the archive has passed validation; a single
/// unsafe or oversized entry fails the whole archive. Per-entry CRC-32 is checked against the
/// value recorded in the archive as an integrity check independent of any caller-side hash
/// verification performed on the extracted files.
///
/// Diagnostic messages for any failure are emitted via `logger` so production failures are
/// debuggable from the SDK log.
/// @return true on success.
bool ExtractZip(const std::filesystem::path& zip_path,
                const std::filesystem::path& destination,
                ILogger& logger,
                const ZipExtractLimits& limits = {});

}  // namespace fl
