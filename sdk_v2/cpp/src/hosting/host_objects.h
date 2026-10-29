// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "api/backend.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <atomic>
#include <memory>
#include <map>
#include <mutex>
#include <string>

namespace fl::hosting {

struct RemoteObjectIdentity {
  std::string incarnation;
  uint64_t connection_generation{};
  uint64_t object{};
};

nlohmann::json EncodeObjectIdentity(const RemoteObjectIdentity& identity);
RemoteObjectIdentity DecodeObjectIdentity(const nlohmann::json& value);

// One authenticated connection's lookup scope. Retiring lookup never destroys resources beneath admitted workers.
class HostObjects {
 public:
  struct SessionResource {
    SessionResource(std::shared_ptr<api::ManagerBackend> manager_in, std::unique_ptr<api::SessionBackend> session_in);
    void AdmitInvocation(uint64_t id, std::function<void()> cancel);
    void CompleteInvocation(uint64_t id) noexcept;
    bool HasInvocation(uint64_t id) const;
    void Retire() noexcept;
    bool IsRetired() const noexcept { return retired_.load(std::memory_order_acquire); }

    std::shared_ptr<api::ManagerBackend> manager;
    std::unique_ptr<api::SessionBackend> implementation;

   private:
    mutable std::mutex cancellation_mutex_;
    std::map<uint64_t, std::shared_ptr<std::function<void()>>> cancellations_;
    std::atomic<bool> retired_{false};
  };
  struct PreflightResource {
    std::shared_ptr<api::ManagerBackend> manager;
    std::unique_ptr<api::PreflightBackend> implementation;
  };

  HostObjects(std::string incarnation, uint64_t connection_generation, std::shared_ptr<api::ManagerBackend> manager);
  ~HostObjects();
  HostObjects(const HostObjects&) = delete;
  HostObjects& operator=(const HostObjects&) = delete;

  RemoteObjectIdentity AddSession(std::unique_ptr<api::SessionBackend> session);
  RemoteObjectIdentity AddPreflight(std::unique_ptr<api::PreflightBackend> preflight);
  std::shared_ptr<SessionResource> Session(const RemoteObjectIdentity& identity) const;
  std::shared_ptr<PreflightResource> Preflight(const RemoteObjectIdentity& identity) const;
  void ReleaseSession(const RemoteObjectIdentity& identity);
  void ReleasePreflight(const RemoteObjectIdentity& identity);

  // Caller cancels/unblocks the owner's invocations separately. Acquired resources retain their full native graph.
  void Close() noexcept;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace fl::hosting
