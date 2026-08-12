// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <mutex>
#include <unordered_set>
#include <vector>

namespace fl {

class Session;

/// Process-wide set of every live Session, maintained by Session's constructor and
/// destructor.
///
/// SessionManager only knows about sessions that took a SessionRegistration, which the
/// web-service handlers do but the direct API does not. Cancellation must reach *every*
/// session: a non-terminating direct-API request is precisely the case that pins the
/// model refcount and stalls manager teardown.
///
/// This tracks sessions for cancellation only. It deliberately has no bearing on
/// SessionManager::WaitForDrain(), because an idle session the caller still owns must
/// not block shutdown.
class LiveSessionRegistry {
 public:
  static LiveSessionRegistry& Instance();

  void Add(Session& session);
  void Remove(Session& session);

  /// Snapshot of the live sessions. Returned by value so callers can cancel without
  /// holding the lock — Session::Cancel() reaches into the inference engine and would
  /// otherwise deadlock against a session unwinding and calling Remove().
  std::vector<Session*> Snapshot() const;

 private:
  LiveSessionRegistry() = default;

  mutable std::mutex mutex_;
  std::unordered_set<Session*> sessions_;
};

}  // namespace fl
