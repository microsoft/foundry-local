// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "ep_detection/ep_bootstrapper.h"
#include "ep_detection/ep_types.h"

#include <string>

namespace fl {

class ILogger;

/// Bootstrapper for the CUDA execution provider.
///
/// Windows x64: downloads CUDA EP binaries from Azure CDN, extracts,
/// verifies SHA256, preloads the GenAI runtime DLL, then registers with ORT via callback.
///
/// Linux x64: registers from co-located .so files (no download needed).
///
/// Checks for NVIDIA GPU externally — only instantiate if NVIDIA GPU detected.
class CudaEpBootstrapper : public IEpBootstrapper {
 public:
  /// @param ep_dir  Base directory for EP packages (e.g., appdata/foundry-local).
  ///                The CUDA package will be at ep_dir/cuda-ep/.
  /// @param register_ep  Callback to register the EP DLL with ORT.
  CudaEpBootstrapper(std::string ep_dir, EpRegistrationCallback register_ep);
  ~CudaEpBootstrapper() override = default;

  // Non-copyable
  CudaEpBootstrapper(const CudaEpBootstrapper&) = delete;
  CudaEpBootstrapper& operator=(const CudaEpBootstrapper&) = delete;

  const std::string& Name() const override;
  bool IsRegistered() const override;
  bool DownloadAndRegister(bool force,
                           const ProgressCallback& progress_cb,
                           ILogger& logger) override;

  /// Check if an NVIDIA GPU with sufficient compute capability is present.
  /// Shells out to nvidia-smi to check. Returns false if nvidia-smi is not found.
  static bool HasNvidiaGpu();

 private:
  std::string ep_dir_;
  std::string name_ = "CUDAExecutionProvider";
  bool registered_ = false;
  int attempts_ = 0;
  EpRegistrationCallback register_ep_;
};

}  // namespace fl
