// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace fl {

/// Normalize model artifact identity using the platform's weak canonicalization, with a lexical fallback.
/// Removes trailing separators and equivalent Windows extended drive/UNC prefixes.
std::string NormalizeModelPath(std::string_view path);

/// Compare normalized artifact paths, case-insensitively on Windows. Neither path needs to exist.
bool ModelPathsEqual(std::string_view left, std::string_view right);

/// Returns true if `candidate` resolves to a path inside (or equal to) `root`.
/// Uses fl::platform::GetWeaklyCanonicalPath so the paths need not exist on
/// disk. This is used to defend against path-traversal where untrusted input
/// contains `..` segments or absolute paths that would escape the destination
/// directory.
bool IsPathWithinDirectory(const std::filesystem::path& candidate,
                           const std::filesystem::path& root);

}  // namespace fl
