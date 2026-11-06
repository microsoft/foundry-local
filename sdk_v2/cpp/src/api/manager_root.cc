// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "api/manager_root.h"

#include "exception.h"
#include "hosting/child_host_role.h"

#if FOUNDRY_LOCAL_HAS_SHARED_HOST
#include "hosting/host_coordinator.h"
#include "hosting/host_server.h"
#include "manager.h"
#endif

#include <optional>

namespace fl::api {

ManagerRoot::ManagerRoot(const Configuration& configuration) {
  auto effective = configuration;
  if (effective.use_shared_host && effective.external_service_url) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "shared hosting cannot use an external service");
  }

#if FOUNDRY_LOCAL_HAS_SHARED_HOST
  std::optional<hosting::ChildHostRole> child;
#endif
  const auto option = effective.additional_options.find(hosting::kChildHostOption);
  if (option != effective.additional_options.end()) {
    if (effective.use_shared_host) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native child host cannot recursively enable shared hosting");
    }

#if FOUNDRY_LOCAL_HAS_SHARED_HOST
    child = hosting::DecodeChildHostRole(option->second);
    if (effective.external_service_url) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native child host cannot use an external service");
    }

    effective.additional_options.erase(option);
#else
    FL_THROW(FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED, "native shared hosting is not supported on this platform");
#endif
  }

  if (effective.use_shared_host) {
#if FOUNDRY_LOCAL_HAS_SHARED_HOST
    effective.use_shared_host = false;
    auto coordinator = std::make_shared<hosting::HostCoordinator>(effective, hosting::ResolveRuntimeAssets());
    auto metadata = CreateEmbeddedBackend(coordinator->EffectiveConfiguration());
    auto connector = [coordinator = std::move(coordinator)](bool acquire) { return coordinator->Connect(acquire); };
    backend_ = CreateHostedBackend(std::move(metadata), std::move(connector));
    return;
#else
    FL_THROW(FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED, "native shared hosting is not supported on this platform");
#endif
  }

  backend_ = CreateEmbeddedBackend(effective);
#if FOUNDRY_LOCAL_HAS_SHARED_HOST
  if (child) {
    auto lifetime =
        std::make_shared<hosting::HostLifetime>(std::chrono::steady_clock::now() + hosting::kFirstOwnerWindow);
    Manager::Instance().SetSharedHostLifetime(lifetime);
    host_ = std::make_unique<hosting::HostServer>(backend_, child->endpoint, std::move(child->profile_key),
                                                  std::move(child->incarnation), child->starter, std::move(lifetime));
  }
#endif
}

ManagerRoot::~ManagerRoot() = default;

void ManagerRoot::Shutdown() {
#if FOUNDRY_LOCAL_HAS_SHARED_HOST
  if (host_) {
    if (!IsShutdownRequested()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native child-host shutdown is managed by client ownership");
    }

    host_->WaitUntilStopped(hosting::HostLifetime::TimePoint::max());
    host_->RethrowFailure();
    return;
  }
#endif

  backend_->Shutdown();
}

bool ManagerRoot::IsShutdownRequested() const {
#if FOUNDRY_LOCAL_HAS_SHARED_HOST
  if (host_ && host_->WaitUntilStopped(std::chrono::steady_clock::now())) {
    return true;
  }
#endif

  return backend_->IsShutdownRequested();
}

}  // namespace fl::api
