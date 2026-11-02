// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "api/backend.h"

#include <memory>

namespace fl::hosting {
class HostServer;
}

namespace fl::api {

// Owns the selected backend and, only in the packaged host process, the service running above that backend.
class ManagerRoot {
 public:
  explicit ManagerRoot(const Configuration& configuration);
  ~ManagerRoot();
  ManagerRoot(const ManagerRoot&) = delete;
  ManagerRoot& operator=(const ManagerRoot&) = delete;

  ManagerBackend& Backend() noexcept { return *backend_; }
  void Shutdown();
  bool IsShutdownRequested() const;

 private:
  std::shared_ptr<ManagerBackend> backend_;
#if FOUNDRY_LOCAL_HAS_SHARED_HOST
  std::unique_ptr<hosting::HostServer> host_;
#endif
};

}  // namespace fl::api
