// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <memory>
#include <mutex>
#include <vector>

namespace fl {

class SessionControl;

/// Process-wide cancellation controls for live Sessions.
///
/// Direct API sessions are not registered with SessionManager, so shutdown uses this
/// registry to cancel every Session. It does not affect SessionManager::WaitForDrain().
/// This registry is cancellation-only: an idle Session still owned by a caller must not block shutdown.
class LiveSessionRegistry {
 public:
  static LiveSessionRegistry& Instance();

  void Add(const std::shared_ptr<SessionControl>& control);
  void Remove(const std::shared_ptr<SessionControl>& control);

  /// Returns shared ownership of each live control. Returned controls remain valid after
  /// their Sessions are destroyed.
  std::vector<std::shared_ptr<SessionControl>> GetLiveControls() const;

 private:
  LiveSessionRegistry() = default;

  mutable std::mutex mutex_;
  std::vector<std::weak_ptr<SessionControl>> controls_;
};

}  // namespace fl
