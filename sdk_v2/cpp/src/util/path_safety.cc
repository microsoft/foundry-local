// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "util/path_safety.h"

#include "platform/path.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace fl {

std::string NormalizeModelPath(std::string_view path) {
  std::filesystem::path normalized;
  std::string error_message;
  if (!platform::GetWeaklyCanonicalPath(std::filesystem::path(std::string(path)), normalized, error_message)) {
    std::error_code ec;
    normalized = std::filesystem::absolute(std::filesystem::path(std::string(path)), ec);
    if (ec) {
      normalized = std::filesystem::path(std::string(path));
    }
  }

  normalized = normalized.lexically_normal();
  if (normalized.has_relative_path() && normalized.filename().empty()) {
    normalized = normalized.parent_path();
  }

#ifdef _WIN32
  const auto& native = normalized.native();
  if (native.starts_with(L"\\\\?\\UNC\\")) {
    normalized = std::filesystem::path(L"\\\\" + native.substr(8));
  } else if (native.starts_with(L"\\\\?\\") && native.size() >= 7 && native[5] == L':') {
    normalized = std::filesystem::path(native.substr(4));
  }
#endif

  return normalized.string();
}

bool ModelPathsEqual(std::string_view left, std::string_view right) {
  const auto normalized_left = NormalizeModelPath(left);
  const auto normalized_right = NormalizeModelPath(right);
#ifdef _WIN32
  return normalized_left.size() == normalized_right.size() &&
         std::equal(normalized_left.begin(), normalized_left.end(), normalized_right.begin(), [](char lhs, char rhs) {
           return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
         });
#else
  return normalized_left == normalized_right;
#endif
}

bool IsPathWithinDirectory(const std::filesystem::path& candidate,
                           const std::filesystem::path& root) {
  std::filesystem::path canonical_candidate;
  std::filesystem::path canonical_root;
  std::string error_message;

  if (!platform::GetWeaklyCanonicalPath(candidate, canonical_candidate, error_message)) {
    return false;
  }

  if (!platform::GetWeaklyCanonicalPath(root, canonical_root, error_message)) {
    return false;
  }

  // Compare path components rather than raw strings to avoid trailing-separator
  // and case-sensitivity edge cases (e.g. "/foo/bar" should not be considered
  // inside "/foo/ba"). path::iterator yields one component at a time.
  auto root_it = canonical_root.begin();
  auto root_end = canonical_root.end();
  auto cand_it = canonical_candidate.begin();
  auto cand_end = canonical_candidate.end();

  for (; root_it != root_end; ++root_it, ++cand_it) {
    if (cand_it == cand_end || *cand_it != *root_it) {
      return false;
    }
  }

  return true;
}

}  // namespace fl
