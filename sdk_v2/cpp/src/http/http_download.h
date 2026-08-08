// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace fl {

class ILogger;

std::optional<int64_t> ParseContentLengthHeader(std::string_view value);

bool ContentLengthMatchesBody(int64_t content_length, int64_t bytes_downloaded);

/// Download a file from an HTTP(S) URL to a local path.
/// Supports progress reporting, cancellation, and an optional size cap.
/// @param url  The URL to download from.
/// @param destination  Local file path to write to.
/// @param user_agent  HTTP User-Agent header.
/// @param cancel_flag  Set to true to cancel. nullptr if not needed.
/// @param progress_cb  Called with percent 0.0-100.0. Empty = no callback.
/// @param logger  Logger for diagnostic output on failure.
/// @param max_bytes  Maximum accepted response size. This bounds disk and network usage if the
///                   endpoint unexpectedly returns a larger body than the artifact manifest allows.
///                   -1 means no cap.
/// @return true on success, false on failure.
bool HttpDownloadFile(const std::string& url, const std::filesystem::path& destination, const std::string& user_agent,
                      std::atomic<bool>* cancel_flag, std::function<void(float percent)> progress_cb, ILogger& logger,
                      int64_t max_bytes = -1);

}  // namespace fl
