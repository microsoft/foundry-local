// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

namespace fl {

class ILogger;

#ifdef _WIN32
/// Keeps EP bundle directories available to the Windows DLL loader for dependencies loaded after
/// provider registration. Provider libraries themselves are still loaded and owned by ORT.
class EpBundleSearchPathOwner {
 public:
  EpBundleSearchPathOwner() = default;
  ~EpBundleSearchPathOwner();

  EpBundleSearchPathOwner(const EpBundleSearchPathOwner&) = delete;
  EpBundleSearchPathOwner& operator=(const EpBundleSearchPathOwner&) = delete;

  bool Add(const std::filesystem::path& directory, std::string_view ep_name, ILogger& logger);

 private:
  std::vector<std::filesystem::path> directories_;
  std::vector<void*> cookies_;
};
#endif

}  // namespace fl
