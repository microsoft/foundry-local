// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fl {

class SessionRuntime;

/// Process-wide set of every live session runtime, populated by MakeSessionRuntime() and pruned by
/// ~SessionRuntime.
///
/// It stores weak references to the stable, shared SessionRuntime — never Session facade addresses — so a
/// Session can move (or be destroyed while an operation still runs) without duplicating or losing its
/// cancellation identity, and so a promoted (strong) snapshot lets shutdown cancel a session even as it
/// unwinds concurrently.
///
/// SessionManager only knows about sessions that took a SessionRegistration, which the web-service handlers
/// do but the direct API does not. Cancellation must reach *every* session: a non-terminating direct-API
/// request is precisely the case that pins the model refcount and stalls manager teardown.
///
/// Admission gate: while admission is closed (a SessionManager shutdown is in progress), a control that
/// registers concurrently is immediately marked terminal, so a session created in the race window after the
/// shutdown snapshot still has its operations rejected. Closure is *owned*, not a bare flag — see
/// AdmissionClosure — so overlapping managers cannot reopen each other's shutdown, while sequential Manager
/// recreation still admits new sessions once the previous manager is gone.
///
/// This tracks controls for cancellation only. It deliberately has no bearing on
/// SessionManager::WaitForDrain(), because an idle session the caller still owns must not block shutdown.
class LiveSessionRegistry {
 public:
  /// Move-only ownership token for one closure of the admission gate.
  ///
  /// The registry is a leaked process-wide singleton shared across Manager lifetimes, so admission state
  /// cannot be a bool that anyone may flip: a second manager's shutdown must not be able to reopen the first
  /// manager's closure, and neither manager's teardown must reopen admission while the other is still
  /// shutting down. Each closure holds a reference on a depth counter and releases exactly once; admission
  /// is open only when no closure is outstanding.
  class AdmissionClosure {
   public:
    AdmissionClosure() = default;
    ~AdmissionClosure() noexcept { Release(); }

    AdmissionClosure(AdmissionClosure&& other) noexcept
        : registry_(other.registry_) {
      other.registry_ = nullptr;
    }

    AdmissionClosure& operator=(AdmissionClosure&& other) noexcept {
      if (this != &other) {
        Release();
        registry_ = other.registry_;
        other.registry_ = nullptr;
      }

      return *this;
    }

    AdmissionClosure(const AdmissionClosure&) = delete;
    AdmissionClosure& operator=(const AdmissionClosure&) = delete;

    /// True while this token still holds admission closed.
    bool Engaged() const { return registry_ != nullptr; }

    /// Reopen this closure's share of the gate. Idempotent.
    void Release() noexcept;

   private:
    friend class LiveSessionRegistry;

    explicit AdmissionClosure(LiveSessionRegistry& registry) : registry_(&registry) {}

    LiveSessionRegistry* registry_ = nullptr;
  };

  static LiveSessionRegistry& Instance();

  /// Track `runtime` weakly. If admission is currently closed, the runtime is marked terminal immediately.
  void Add(const std::shared_ptr<SessionRuntime>& runtime);

  /// Stop tracking `runtime`. Safe to call for an untracked runtime.
  void Remove(SessionRuntime* runtime) noexcept;

  /// Promoted snapshot of the live runtimes. Returned by value so callers can cancel without holding the
  /// lock — CancelAll() reaches into the inference engine and would otherwise deadlock against a session
  /// unwinding and calling Remove(). Expired weak entries are pruned.
  std::vector<std::shared_ptr<SessionRuntime>> Snapshot() const;

  /// Close admission for as long as the returned token lives. Runtimes registered while any token is
  /// outstanding are marked terminal on arrival.
  [[nodiscard]] AdmissionClosure CloseAdmission();

  /// True while no closure token is outstanding.
  bool IsAdmissionOpen() const;

 private:
  LiveSessionRegistry() = default;

  /// Drop one closure reference. Called only by AdmissionClosure.
  void ReleaseAdmissionClosure() noexcept;

  mutable std::mutex mutex_;
  // Mutable so Snapshot() (logically const) can prune entries whose runtime expired without a Remove().
  mutable std::unordered_map<SessionRuntime*, std::weak_ptr<SessionRuntime>> runtimes_;
  size_t closure_depth_ = 0;
};

}  // namespace fl
