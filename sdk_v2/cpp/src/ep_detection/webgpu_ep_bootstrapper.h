// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "ep_detection/ep_bootstrapper.h"
#include "ep_detection/ep_types.h"

#include <string>

namespace fl {

class ILogger;

/// Bootstrapper for the WebGPU execution provider.
///
/// Uses platform-specific package metadata (download URL and SHA-256 hash),
/// downloads the binary, verifies integrity, then registers with ORT.
///
/// Supports Windows x64/ARM64, Linux x64, and macOS ARM64.
class WebGpuEpBootstrapper : public IEpBootstrapper {
 public:
  /// @param ep_dir  Base directory for EP packages (e.g., appdata/foundry-local).
  ///                The WebGPU package will be at ep_dir/webgpu-ep/.
  /// @param register_ep  Callback to register the EP DLL with ORT.
  WebGpuEpBootstrapper(std::string ep_dir, EpRegistrationCallback register_ep);
  ~WebGpuEpBootstrapper() override = default;

  // Non-copyable
  WebGpuEpBootstrapper(const WebGpuEpBootstrapper&) = delete;
  WebGpuEpBootstrapper& operator=(const WebGpuEpBootstrapper&) = delete;

  const std::string& Name() const override;
  bool IsRegistered() const override;
  bool DownloadAndRegister(bool force,
                           const ProgressCallback& progress_cb,
                           ILogger& logger) override;

 private:
  std::string ep_dir_;
  std::string name_ = "WebGpuExecutionProvider";
  bool registered_ = false;
  int attempts_ = 0;
  EpRegistrationCallback register_ep_;
};

}  // namespace fl
