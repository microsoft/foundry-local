// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <memory>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <utility>

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
namespace oatpp::web::server {
class HttpRequestHandler;
class HttpConnectionHandler;
}  // namespace oatpp::web::server
#endif

namespace fl {
struct WebServiceActivity {
  std::uint64_t active_requests{};
  std::uint64_t idle_milliseconds{};
};

// The state outlives the service when a response or streaming worker still owns its admission.
class HttpActivityTracker {
 public:
  WebServiceActivity Snapshot() const {
    std::lock_guard lock(state_->mutex);
    return {state_->active,
            state_->active
                ? 0u
                : static_cast<std::uint64_t>(
                      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - state_->completed).count())};
  }

  std::shared_ptr<void> Begin() {
    std::lock_guard lock(state_->mutex);
    if (state_->closed) {
      return {};
    }

    auto work = std::make_shared<Work>(state_);
    ++state_->active;
    return work;
  }

  bool TryBeginStop(std::uint64_t minimum_idle_milliseconds) {
    std::lock_guard lock(state_->mutex);
    const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - state_->completed).count();
    if (state_->active || static_cast<std::uint64_t>(idle) < minimum_idle_milliseconds) {
      return false;
    }

    state_->closed = true;
    return true;
  }

  void CancelStop() {
    std::lock_guard lock(state_->mutex);
    state_->closed = false;
  }

  void StartBaseline() {
    std::lock_guard lock(state_->mutex);
    state_->completed = Clock::now();
    state_->closed = false;
  }

 private:
  using Clock = std::chrono::steady_clock;
  struct State {
    std::mutex mutex;
    std::uint64_t active{};
    bool closed{};
    Clock::time_point completed = Clock::now();
  };
  struct Work {
    explicit Work(std::shared_ptr<State> state) : state(std::move(state)) {}
    ~Work() {
      std::lock_guard lock(state->mutex);
      if (--state->active == 0) {
        state->completed = Clock::now();
      }
    }
    const std::shared_ptr<State> state;
  };
  const std::shared_ptr<State> state_ = std::make_shared<State>();
};

namespace hosting {
class HostLifetime;
}

// Propagates HTTP activity and shared-host admission to workers started while a handler executes.
class HttpRequestLifetime {
 public:
  explicit HttpRequestLifetime(std::shared_ptr<void> work) noexcept
      : previous_(std::exchange(current_, std::move(work))) {}
  ~HttpRequestLifetime() { current_ = std::move(previous_); }
  HttpRequestLifetime(const HttpRequestLifetime&) = delete;
  HttpRequestLifetime& operator=(const HttpRequestLifetime&) = delete;

  static std::shared_ptr<void> CurrentWork() noexcept { return current_; }

 private:
  inline static thread_local std::shared_ptr<void> current_;
  std::shared_ptr<void> previous_;
};

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
void TrackHttpActivity(oatpp::web::server::HttpConnectionHandler& handler,
                       std::shared_ptr<HttpActivityTracker> activity);
std::shared_ptr<oatpp::web::server::HttpRequestHandler> WithSharedHostAdmission(
    std::shared_ptr<oatpp::web::server::HttpRequestHandler> handler, std::shared_ptr<hosting::HostLifetime> lifetime);
#endif

}  // namespace fl
