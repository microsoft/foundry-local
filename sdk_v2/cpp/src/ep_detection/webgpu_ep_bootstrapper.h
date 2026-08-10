// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "ep_detection/ep_bootstrapper.h"
#include "ep_detection/ep_bundle_installer.h"
#include "ep_detection/ep_types.h"

#include <filesystem>
#include <string>

namespace fl {

class ILogger;

/// Bootstrapper for the WebGPU execution provider.
///
/// Installs and registers the WebGPU execution provider.
class WebGpuEpBootstrapper : public IEpBootstrapper {
 public:
  /// @param root_dir  Root directory for the WebGPU EP bundle, e.g. "<app_data_dir>/ep/webgpu-ep".
  /// @param register_ep  Callback to register the EP DLL with ORT.
  WebGpuEpBootstrapper(std::string root_dir, EpRegistrationCallback register_ep,
                       EpBundleManifestFactory manifest_factory = nullptr,
                       EpArtifactDownloadFn download_fn = nullptr);
  ~WebGpuEpBootstrapper() override = default;

  // Non-copyable
  WebGpuEpBootstrapper(const WebGpuEpBootstrapper&) = delete;
  WebGpuEpBootstrapper& operator=(const WebGpuEpBootstrapper&) = delete;

  const std::string& Name() const override;
  bool IsRegistered() const override;
  bool DownloadAndRegister(bool force, const ProgressCallback& progress_cb, ILogger& logger) override;
  bool PrepareForModelLoad(ILogger& logger) override;

  /// Whether Foundry Local publishes a WebGPU EP bundle for this platform.
  static bool IsSupportedPlatform();

 private:
  std::string name_ = "WebGpuExecutionProvider";
  bool registered_ = false;
  int attempts_ = 0;
  EpRegistrationCallback register_ep_;
  EpBundleManifestFactory manifest_factory_;
  EpBundleInstaller installer_;
  std::filesystem::path bundle_dir_;
};

}  // namespace fl
