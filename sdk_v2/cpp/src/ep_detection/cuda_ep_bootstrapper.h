// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "ep_detection/ep_bootstrapper.h"
#include "ep_detection/ep_bundle_installer.h"
#include "ep_detection/ep_types.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fl {

class ILogger;

#if defined(__linux__)
using CudaGenAiDependencyLoader =
    std::function<std::shared_ptr<void>(const std::filesystem::path&, ILogger&)>;
#endif

/// Bootstrapper for the CUDA execution provider.
///
/// Installs and registers the CUDA execution provider.
class CudaEpBootstrapper : public IEpBootstrapper {
 public:
  /// @param root_dir  Root directory for the CUDA EP bundle, e.g. "<app_data_dir>/ep/cuda-ep".
  /// @param register_ep  Callback to register the EP DLL with ORT.
  CudaEpBootstrapper(std::string root_dir, EpRegistrationCallback register_ep,
                     EpBundleManifestFactory manifest_factory = nullptr,
                     EpArtifactDownloadFn download_fn = nullptr
#if defined(__linux__)
                     ,
                     CudaGenAiDependencyLoader genai_cuda_loader = nullptr
#endif
  );
  ~CudaEpBootstrapper() override;

  // Non-copyable
  CudaEpBootstrapper(const CudaEpBootstrapper&) = delete;
  CudaEpBootstrapper& operator=(const CudaEpBootstrapper&) = delete;

  const std::string& Name() const override;
  bool IsRegistered() const override;
  bool DownloadAndRegister(bool force, const ProgressCallback& progress_cb, ILogger& logger) override;
  bool PrepareForModelLoad(ILogger& logger) override;

  /// Check for an NVIDIA GPU with compute capability >= 5.0 using NVML.
  static bool HasNvidiaGpu(ILogger& logger);

  /// Whether Foundry Local publishes a CUDA EP bundle for this platform.
  static bool IsSupportedPlatform();

 private:
  std::string name_ = "CUDAExecutionProvider";
  bool registered_ = false;
  int attempts_ = 0;
  EpRegistrationCallback register_ep_;
  EpBundleManifestFactory manifest_factory_;
  EpBundleInstaller installer_;
  std::filesystem::path bundle_dir_;
#if defined(__linux__)
  std::vector<std::pair<std::filesystem::path, std::shared_ptr<void>>> genai_cuda_libraries_;
  CudaGenAiDependencyLoader genai_cuda_loader_;
#endif
};

}  // namespace fl
