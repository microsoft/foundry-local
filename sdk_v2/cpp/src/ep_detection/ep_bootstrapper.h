// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <functional>
#include <string>

namespace fl {

class ILogger;

inline constexpr float kEpReadyToRegisterProgress = 90.0f;

/// Interface for a single execution provider bootstrapper.
/// Each bootstrapper manages discovery, download, and registration of one EP.
class IEpBootstrapper {
 public:
  virtual ~IEpBootstrapper() = default;

  /// Human-readable EP name (e.g., "CUDAExecutionProvider", "QNNExecutionProvider").
  virtual const std::string& Name() const = 0;

  /// Whether this EP has been successfully registered with the ORT environment.
  virtual bool IsRegistered() const = 0;

  /// Returns true to continue, false to cancel.
  using ProgressCallback = std::function<bool(const std::string& ep_name, float percent)>;

  /// Downloads (if needed) and registers the EP with ORT.
  /// @param force  Re-download even if already registered.
  /// @param progress_cb  Called with (ep_name, percent 0.0-100.0). Returns false to cancel.
  /// @param logger  Logger for diagnostic output during download/registration.
  /// @return true on success, false on failure or cancellation.
  virtual bool DownloadAndRegister(bool force,
                                   const ProgressCallback& progress_cb,
                                   ILogger& logger) = 0;

  /// Prepare process state needed to load a model with this EP.
  virtual bool PrepareForModelLoad(ILogger& /*logger*/) { return true; }
};

}  // namespace fl
