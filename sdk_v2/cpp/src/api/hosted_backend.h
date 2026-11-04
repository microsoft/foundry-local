// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "api/backend.h"
#include "hosting/host_objects.h"
#include "hosting/sdk_connection.h"

#include <atomic>

namespace fl::api {

// A manager's authenticated connection. Closing the manager invalidates children without rebinding or replay.
class HostedContext {
 public:
  HostedContext(std::shared_ptr<hosting::SdkConnection> connection, std::string incarnation, uint64_t generation);
  ~HostedContext();
  HostedContext(const HostedContext&) = delete;
  HostedContext& operator=(const HostedContext&) = delete;

  hosting::SdkConnection& Connection() const;
  void Validate(const hosting::RemoteObjectIdentity& identity) const;
  void Close() noexcept;
  void Release(hosting::Operation operation, const hosting::RemoteObjectIdentity& identity) noexcept;
  nlohmann::json Call(hosting::Operation operation, const nlohmann::json& payload) const;

 private:
  const std::shared_ptr<hosting::SdkConnection> connection_;
  const std::string incarnation_;
  const uint64_t generation_;
  std::atomic<bool> closed_{false};
};

std::unique_ptr<SessionBackend> CreateHostedSession(std::shared_ptr<HostedContext> context,
                                                    hosting::RemoteObjectIdentity identity,
                                                    std::shared_ptr<void> owner = {});

// Observe never starts a host or acquires an owner. Acquire attaches/starts and returns the manager's owner connection.
// Absence is represented by null only for observation; incompatibility and failed observations must throw.
using HostedConnector = std::function<std::shared_ptr<HostedContext>(bool acquire)>;

std::unique_ptr<ManagerBackend> CreateHostedBackend(std::shared_ptr<ManagerBackend> metadata,
                                                    HostedConnector connector);

}  // namespace fl::api
