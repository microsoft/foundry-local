// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>

namespace fl::hosting {

struct ProcessIdentity {
  std::uint64_t pid{};
  // Opaque, boot-local OS creation identity. Capture in the owning process, not from socket EOF or a PID alone.
  std::uint64_t generation{};

  bool operator==(const ProcessIdentity&) const = default;
};

enum class ProcessWatchFailure { kStaleIdentity,
                                 kObservationFailed,
                                 kUnsupported };

class ProcessWatchError : public std::system_error {
 public:
  ProcessWatchError(ProcessWatchFailure failure, std::error_code error, const std::string& operation)
      : std::system_error(error, operation), failure_(failure) {}

  [[nodiscard]] ProcessWatchFailure Failure() const noexcept { return failure_; }

 private:
  ProcessWatchFailure failure_;
};

[[nodiscard]] ProcessIdentity CurrentProcessIdentity();

enum class ProcessIdentityProbe { kMatching,
                                  kAbsent };

// Authenticate the predecessor record first. Absent means the OS reports no such PID or a different generation;
// it never authorizes killing a reused PID. Matching is a point-in-time identity check, not a liveness guarantee.
// Access, unsupported and uncertain failures throw ProcessWatchError; invalid inputs throw invalid_argument.
// This reconciliation check neither acquires a startup lock nor replaces exit observation on an existing ProcessWatch.
[[nodiscard]] ProcessIdentityProbe ProbeProcessIdentity(ProcessIdentity identity);

// Authenticate the OS peer PID before passing its self-captured generation here. No IPC dependency or PID fallback.
// Invalid identities throw invalid_argument. Missing/reused processes throw kStaleIdentity, NOT proof of observed exit.
// Access/resource failures and unsupported observation throw separately; preserve reconciliation state on failure.
// Operations on one watch require external synchronization, including move/destruction. A moved-from watch is unusable.
class ProcessWatch {
 public:
  using TimePoint = std::chrono::steady_clock::time_point;

  explicit ProcessWatch(ProcessIdentity identity);
  ~ProcessWatch();
  ProcessWatch(ProcessWatch&&) noexcept;
  ProcessWatch& operator=(ProcessWatch&&) noexcept;
  ProcessWatch(const ProcessWatch&) = delete;
  ProcessWatch& operator=(const ProcessWatch&) = delete;

  [[nodiscard]] ProcessIdentity Identity() const noexcept { return identity_; }
  [[nodiscard]] bool IsExited();
  // True only after OS exit notification; false means the deadline elapsed without observing exit.
  [[nodiscard]] bool WaitUntil(TimePoint deadline);

 private:
  struct State;
  ProcessIdentity identity_;
  std::unique_ptr<State> state_;
};

}  // namespace fl::hosting
