// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/live_session_registry.h"

namespace fl {

LiveSessionRegistry& LiveSessionRegistry::Instance() {
  // Leaked intentionally: sessions may be destroyed during static destruction, and a
  // destroyed registry would make Remove() a use-after-free.
  static LiveSessionRegistry* instance = new LiveSessionRegistry();
  return *instance;
}

void LiveSessionRegistry::Add(Session& session) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.insert(&session);
}

void LiveSessionRegistry::Remove(Session& session) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.erase(&session);
}

std::vector<Session*> LiveSessionRegistry::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return {sessions_.begin(), sessions_.end()};
}

}  // namespace fl
