// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/live_session_registry.h"

#include "inferencing/session/session_control.h"

namespace fl {

LiveSessionRegistry& LiveSessionRegistry::Instance() {
  // Leaked intentionally: sessions may be destroyed during static destruction, and a
  // destroyed registry would make Remove() a use-after-free.
  static LiveSessionRegistry* instance = new LiveSessionRegistry();
  return *instance;
}

void LiveSessionRegistry::Add(const std::shared_ptr<SessionControl>& control) {
  std::lock_guard<std::mutex> lock(mutex_);
  controls_.emplace_back(control);
}

void LiveSessionRegistry::Remove(const std::shared_ptr<SessionControl>& control) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::erase_if(controls_, [&](const std::weak_ptr<SessionControl>& candidate) {
    const auto live_control = candidate.lock();
    return !live_control || live_control == control;
  });
}

std::vector<std::shared_ptr<SessionControl>> LiveSessionRegistry::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<std::shared_ptr<SessionControl>> controls;
  controls.reserve(controls_.size());

  for (const auto& weak_control : controls_) {
    if (auto control = weak_control.lock()) {
      controls.push_back(std::move(control));
    }
  }

  return controls;
}

}  // namespace fl
