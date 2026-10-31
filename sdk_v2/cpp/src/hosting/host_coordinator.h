// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "api/hosted_backend.h"
#include "hosting/host_profile.h"
#include "hosting/host_state.h"
#include "hosting/runtime_assets.h"

namespace fl::hosting {

class HostCoordinator {
 public:
  HostCoordinator(const Configuration& configuration, RuntimeAssets assets, std::filesystem::path root = {},
                  std::chrono::milliseconds startup_timeout = std::chrono::seconds{30});
  std::shared_ptr<api::HostedContext> Connect(bool acquire) const;
  const Configuration& EffectiveConfiguration() const noexcept { return profile_.configuration; }
  const HostStateStore& Store() const noexcept { return store_; }

 private:
  std::shared_ptr<api::HostedContext> Attach(const HostLaunchRecord& record, bool acquire,
                                             ProcessWatch::TimePoint deadline) const;
  const RuntimeAssets assets_;
  const HostProfile profile_;
  const HostStateStore store_;
  const std::chrono::milliseconds startup_timeout_;
};

}  // namespace fl::hosting
