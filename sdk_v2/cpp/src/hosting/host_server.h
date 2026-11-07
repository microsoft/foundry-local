// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "api/backend.h"
#include "hosting/host_lifetime.h"
#include "hosting/process_watch.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace fl::hosting {

// Runs inside the native runtime, after the dependency-light executable has bootstrapped the embedded manager.
// The executable must retain this service until all workers have joined, including after shutdown is requested.
class HostServer {
 public:
  using Clock = std::function<HostLifetime::TimePoint()>;

  static constexpr std::size_t kMaxConnections = 64;
  static constexpr std::size_t kMaxConcurrentCalls = 64;

  HostServer(std::shared_ptr<api::ManagerBackend> manager, const std::filesystem::path& endpoint,
             std::string profile_key, std::string incarnation, ProcessIdentity starter,
             std::shared_ptr<HostLifetime> lifetime, Clock clock = std::chrono::steady_clock::now);
  ~HostServer();
  HostServer(const HostServer&) = delete;
  HostServer& operator=(const HostServer&) = delete;

  HostLifetime& Lifetime() noexcept;
  bool WaitUntilStopped(HostLifetime::TimePoint deadline);
  // Fatal listener/monitor/native-shutdown failures are retained; ordinary peer failures remain connection-local.
  void RethrowFailure() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fl::hosting
