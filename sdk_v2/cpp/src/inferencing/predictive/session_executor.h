// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/predictive/non_generative_runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace fl::detail {

inline constexpr size_t kMaximumQueuedPredictiveRequests = 64;

template <typename Session>
class SessionExecutor {
 public:
  using Factory = std::function<std::unique_ptr<Session>()>;

  explicit SessionExecutor(Factory factory)
      : thread_([this, factory = std::move(factory)]() mutable {
          try {
            session_ = factory();
          } catch (...) {
            startup_error_ = std::current_exception();
          }
          {
            std::lock_guard lock(mutex_);
            ready_ = true;
          }
          condition_.notify_all();
          if (startup_error_) return;

          std::unique_lock lock(mutex_);
          while (true) {
            condition_.wait(lock,
                            [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) break;
            auto task = std::move(tasks_.front());
            tasks_.pop_front();
            lock.unlock();
            task(*session_);
            lock.lock();
          }
          session_.reset();
        }) {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return ready_; });
    if (startup_error_) {
      lock.unlock();
      thread_.join();
      std::rethrow_exception(startup_error_);
    }
  }

  ~SessionExecutor() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_one();
    thread_.join();
  }

  SessionExecutor(const SessionExecutor&) = delete;
  SessionExecutor& operator=(const SessionExecutor&) = delete;

  bool WaitForPendingTasks(size_t count,
                           std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(
        lock, timeout, [&] { return tasks_.size() >= count; });
  }

  template <typename Function>
  auto Invoke(Function&& function, std::function<bool()> is_canceled = {})
      -> std::invoke_result_t<Function, Session&> {
    using Result = std::invoke_result_t<Function, Session&>;
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    auto state = std::make_shared<std::atomic<int>>(0);
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        throw std::runtime_error("typed runtime is stopping");
      if (tasks_.size() >= kMaximumQueuedPredictiveRequests)
        throw RuntimeCapacityError("typed runtime request queue is full");
      tasks_.emplace_back(
          [promise, state, function = std::forward<Function>(function),
           is_canceled](Session& session) mutable {
            try {
              int queued = 0;
              if (!state->compare_exchange_strong(queued, 1) ||
                  (is_canceled && is_canceled()))
                throw std::runtime_error(
                    "non-generative request was canceled before execution");
              if constexpr (std::is_void_v<Result>) {
                std::invoke(function, session);
                promise->set_value();
              } else {
                promise->set_value(std::invoke(function, session));
              }
            } catch (...) {
              promise->set_exception(std::current_exception());
            }
          });
    }
    condition_.notify_all();
    while (future.wait_for(std::chrono::milliseconds(10)) !=
           std::future_status::ready) {
      if (is_canceled && is_canceled()) {
        int queued = 0;
        if (state->compare_exchange_strong(queued, 2))
          throw std::runtime_error(
              "non-generative request was canceled before execution");
      }
    }
    return future.get();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::function<void(Session&)>> tasks_;
  std::unique_ptr<Session> session_;
  std::exception_ptr startup_error_;
  bool ready_{};
  bool stopping_{};
  std::thread thread_;
};

}  // namespace fl::detail
