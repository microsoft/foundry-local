// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace fl::hosting {

class HostLifetime {
  struct SharedState;

 public:
  using TimePoint = std::chrono::steady_clock::time_point;
  // Authenticated connection-generation tokens supplied by transport, not reusable process IDs.
  using OwnerId = std::uint64_t;

  static constexpr auto kReconnectGrace = std::chrono::seconds{5};

  enum class State {
    kAwaitingFirstOwner,
    kAccepting,
    kDraining,
    kGrace,
    kShutdownCommitted,
  };

  struct Snapshot {
    State state;
    std::size_t owner_count;
    std::size_t ipc_work_count;
    std::size_t http_work_count;
    std::optional<TimePoint> deadline;
  };

  // Protects admission accounting only. Workers must separately retain their complete native dependency graph.
  // Different guards may be used concurrently; access to the same guard requires external synchronization.
  class WorkGuard {
   public:
    WorkGuard() noexcept = default;
    ~WorkGuard();

    WorkGuard(const WorkGuard&) = delete;
    WorkGuard& operator=(const WorkGuard&) = delete;
    WorkGuard(WorkGuard&& other) noexcept;
    WorkGuard& operator=(WorkGuard&& other) noexcept;

    explicit operator bool() const noexcept;
    std::optional<OwnerId> Owner() const noexcept;

    // Idempotent. Complete only after execution, callbacks and transport workers no longer need this admission.
    void Complete(TimePoint now) noexcept;

   private:
    friend class HostLifetime;
    WorkGuard(std::shared_ptr<SharedState> state, std::optional<OwnerId> owner, bool stopping_http = false) noexcept;
    void Release(std::optional<TimePoint> now) noexcept;

    std::shared_ptr<SharedState> state_;
    std::optional<OwnerId> owner_;
    bool stopping_http_ = false;
  };

  explicit HostLifetime(TimePoint startup_deadline);
  ~HostLifetime();

  HostLifetime(const HostLifetime&) = delete;
  HostLifetime& operator=(const HostLifetime&) = delete;
  HostLifetime(HostLifetime&&) = delete;
  HostLifetime& operator=(HostLifetime&&) = delete;

  // Duplicate attachment is successful without another reference. Removed IDs must never be reused.
  [[nodiscard]] bool AddOwner(OwnerId owner, TimePoint now);
  // Reports whether removal occurred, so transport can cancel that owner's invocations outside this controller.
  [[nodiscard]] bool RemoveOwner(OwnerId owner, TimePoint now);
  // A confirmed dead starter retires only an unused startup; attachment and this commitment are serialized.
  [[nodiscard]] bool RetireUnusedStartup(TimePoint now);

  // An empty guard means admission was rejected. HTTP work has no owner identity.
  [[nodiscard]] WorkGuard TryAdmitIpcWork(OwnerId owner, TimePoint now);
  [[nodiscard]] WorkGuard TryAdmitHttpWork(TimePoint now);
  // Stop linearizes only with the sole owner and no HTTP work. Block new HTTP admissions until the stop finishes.
  [[nodiscard]] WorkGuard TryAdmitWebServiceStop(OwnerId owner, TimePoint now);

  // Observes time without acquiring ownership. At or after a deadline, commitment wins over attachment.
  // Concurrent, out-of-order time samples are clamped to the latest observation.
  // Untimed guard destruction starts grace at the next observation, never before work actually completes.
  [[nodiscard]] Snapshot Poll(TimePoint now);

 private:
  WorkGuard TryAdmitWork(std::optional<OwnerId> owner, TimePoint now, bool stopping_http = false);

  const std::shared_ptr<SharedState> state_;
};

}  // namespace fl::hosting
