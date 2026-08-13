// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/live_session_registry.h"

#include "inferencing/session/session_runtime.h"

namespace fl {

void LiveSessionRegistry::AdmissionClosure::Release() noexcept {
  if (registry_ == nullptr) {
    return;
  }

  registry_->ReleaseAdmissionClosure();
  registry_ = nullptr;
}

LiveSessionRegistry& LiveSessionRegistry::Instance() {
  // Leaked intentionally: sessions may be destroyed during static destruction, and a destroyed registry
  // would make Remove() a use-after-free.
  static LiveSessionRegistry* instance = new LiveSessionRegistry();
  return *instance;
}

void LiveSessionRegistry::Add(const std::shared_ptr<SessionRuntime>& runtime) {
  std::lock_guard<std::mutex> lock(mutex_);
  runtimes_[runtime.get()] = runtime;

  // Close the shutdown race: a session that registers while a shutdown holds admission closed must not slip
  // past the cancellation snapshot. MarkTerminal only flips a flag and wakes the admission CV — no ORT and
  // no logging under this lock.
  if (closure_depth_ > 0) {
    runtime->MarkTerminal();
  }
}

void LiveSessionRegistry::Remove(SessionRuntime* runtime) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  runtimes_.erase(runtime);
}

std::vector<std::shared_ptr<SessionRuntime>> LiveSessionRegistry::Snapshot() const {
  std::vector<std::shared_ptr<SessionRuntime>> alive;

  std::lock_guard<std::mutex> lock(mutex_);
  alive.reserve(runtimes_.size());

  for (auto it = runtimes_.begin(); it != runtimes_.end();) {
    if (auto strong = it->second.lock()) {
      alive.push_back(std::move(strong));
      ++it;
    } else {
      it = runtimes_.erase(it);  // prune a runtime that expired between registration and this snapshot
    }
  }

  return alive;
}

LiveSessionRegistry::AdmissionClosure LiveSessionRegistry::CloseAdmission() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++closure_depth_;
  }

  return AdmissionClosure(*this);
}

void LiveSessionRegistry::ReleaseAdmissionClosure() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closure_depth_ > 0) {
    --closure_depth_;
  }
}

bool LiveSessionRegistry::IsAdmissionOpen() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return closure_depth_ == 0;
}

}  // namespace fl
