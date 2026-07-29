// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// This translation unit is only compiled when the WinML EP catalog NuGet
// package was resolved at CMake time (WinMLEpCatalog_FOUND, which also sets
// the C++ macro FOUNDRY_LOCAL_HAS_EP_CATALOG=1). Source-list gating happens
// in sdk_v2/cpp/CMakeLists.txt; all C++ callers must guard references on
// FOUNDRY_LOCAL_HAS_EP_CATALOG.
#pragma once

#include "ep_detection/ep_bootstrapper.h"
#include "ep_detection/ep_types.h"

#include <WinMLEpCatalog.h>

#include <memory>
#include <string>
#include <vector>

namespace fl {

class ILogger;

/// Bootstrapper for a single WinML-based execution provider.
/// Each instance wraps one WinMLEpHandle from the WinML EP catalog.
///
/// Code path works on Windows 10 19H1+ (build 18362) — the minimum OS
/// for the bundled WinML 2.x redist DLL (Microsoft.Windows.AI.MachineLearning).
/// Actual EP discovery returns providers only on Windows 11 24H2+ (build 26100),
/// where the OS-delivered EP catalog is populated via Windows Update / Store.
class WinMLEpBootstrapper : public IEpBootstrapper {
 public:
  ~WinMLEpBootstrapper() override = default;

  // Non-copyable
  WinMLEpBootstrapper(const WinMLEpBootstrapper&) = delete;
  WinMLEpBootstrapper& operator=(const WinMLEpBootstrapper&) = delete;

  // Movable (for vector storage)
  WinMLEpBootstrapper(WinMLEpBootstrapper&&) noexcept = default;
  WinMLEpBootstrapper& operator=(WinMLEpBootstrapper&&) noexcept = default;

  const std::string& Name() const override;
  bool IsRegistered() const override;
  bool DownloadAndRegister(bool force,
                           const ProgressCallback& progress_cb,
                           ILogger& logger) override;

  /// Discovers all WinML EPs available on this system.
  /// Returns empty on unsupported OS version or missing WinML DLL.
  /// @param register_ep  Callback called after EnsureReady to register the EP with ORT.
  /// @param logger  Logger for diagnostic output.
  static std::vector<std::unique_ptr<WinMLEpBootstrapper>> DiscoverProviders(
      EpRegistrationCallback register_ep,
      ILogger& logger);

 private:
  std::string name_;
  std::string library_path_;
  bool registered_ = false;
  EpRegistrationCallback register_ep_;

  // Shared across all bootstrappers from the same DiscoverProviders() call
  // to keep the catalog alive until the last bootstrapper is destroyed.
  std::shared_ptr<void> catalog_ref_;
  // Owned by the catalog (no per-EP release in the WinML C API), so this
  // raw pointer is valid as long as catalog_ref_ outlives it. Per-EP
  // cleanup is intentionally absent — do not add Release-like calls here.
  WinMLEpHandle ep_handle_ = nullptr;

  WinMLEpBootstrapper(std::string name, EpRegistrationCallback register_ep,
                      std::shared_ptr<void> catalog_ref, WinMLEpHandle ep_handle);
};

}  // namespace fl
