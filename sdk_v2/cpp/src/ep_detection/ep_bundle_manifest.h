// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fl {

struct EpBundleFile {
  std::string relative_path;
  std::string sha256;
};

struct EpBundleArtifact {
  std::string id;
  std::string url;
  bool is_archive = true;

  std::string archive_sha256;
  std::vector<EpBundleFile> extracted_files;
  std::vector<std::string> ignored_archive_paths;
  uint64_t archive_max_bytes = 0;

  std::string raw_relative_path;
  std::string raw_sha256;
  uint64_t raw_max_bytes = 0;

  bool IsEnabled() const { return !url.empty(); }
};

struct EpBundleManifest {
  std::string bundle_id;
  std::vector<EpBundleArtifact> artifacts;
  std::string provider_relative_path;

  bool IsSupported() const {
    if (bundle_id.empty() || artifacts.empty() || provider_relative_path.empty()) {
      return false;
    }

    for (const auto& artifact : artifacts) {
      if (!artifact.IsEnabled()) {
        return false;
      }
    }

    return true;
  }
};

}  // namespace fl
