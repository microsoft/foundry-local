// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "api/backend.h"
#include "hosting/host_lifetime.h"
#include "hosting/sdk_connection.h"

#include <mutex>

namespace fl::hosting {

// Shared across authenticated connections. Ordinary admission and HTTP shutdown protection belong to the host loop.
class ManagerDispatcher {
 public:
  ManagerDispatcher(std::shared_ptr<api::ManagerBackend> manager, HostLifetime& lifetime);

  static bool Handles(Operation operation) noexcept;
  void Dispatch(const std::shared_ptr<SdkConnection::IncomingCall>& call, std::optional<HostLifetime::OwnerId> owner);

 private:
  nlohmann::json Execute(SdkConnection::IncomingCall& call, std::optional<HostLifetime::OwnerId> owner);

  const std::shared_ptr<api::ManagerBackend> manager_;
  HostLifetime& lifetime_;
  std::mutex web_mutex_;
};

}  // namespace fl::hosting
