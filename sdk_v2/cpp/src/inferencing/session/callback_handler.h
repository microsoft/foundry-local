// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cassert>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include <fmt/format.h>
#include <foundry_local/foundry_local_c.h>

#include "inferencing/session/request.h"
#include "items/item_queue.h"
#include "logger.h"

namespace fl {

/// Per-request streaming callback handler.
///
/// Created by Session::CreateCallbackHandler() at the start of ProcessRequestImpl
/// and destroyed (via unique_ptr) when ProcessRequestImpl returns.
///
/// Items are pushed to the ItemQueue on the caller's thread (preserving
/// generation order). A single worker thread pops items and fires the
/// user callback. This decouples token generation from callback speed
/// while guaranteeing delivery order.
///
/// On cancellation, a backlog within the normal 64-item backpressure window is delivered. A larger backlog is
/// discarded so cancellation cannot be delayed indefinitely by a stalled consumer.
///
/// The Request reference is bound at construction — no need to pass it per push.
/// Destruction drains the queue and joins the worker thread (RAII).
struct CallbackHandler {
  using CallbackFn = std::function<int(flStreamingCallbackData, void*)>;
  static constexpr size_t kMaxCancellationDrainItems = 64;

  CallbackHandler(const Request& request, CallbackFn callback_fn, ILogger& logger,
                  void* user_data = nullptr)
      : request_(request),
        fn_(std::move(callback_fn)),
        user_data_(user_data),
        logger_(logger),
        queue_(std::make_unique<ItemQueue>()) {
    assert(fn_ && "Streaming callback cannot be null");
    data_.version = FOUNDRY_LOCAL_API_VERSION;
    data_.item_queue = queue_->AsApiType();
    worker_ = std::thread(&CallbackHandler::WorkerLoop, this);
  }

  ~CallbackHandler() {
    Drain();
  }

  CallbackHandler(const CallbackHandler&) = delete;
  CallbackHandler& operator=(const CallbackHandler&) = delete;

  /// Push an item into the queue and wake the worker.
  /// Called from the generator thread — returns immediately.
  void PushItem(std::unique_ptr<Item> item) {
    if (request_.IsCancellationRequested()) {
      return;
    }

    queue_->Push(std::move(item));
  }

  /// Wait until every currently queued callback has returned without closing the queue.
  /// The producer may publish more items afterward; unlike Drain(), this is not a terminal operation.
  void DrainPending() {
    std::unique_lock<std::mutex> lock(callback_mutex_);
    callback_cv_.wait(lock, [this] {
      return !callback_in_progress_ && queue_->Size() == 0;
    });
  }

  /// Mark the queue as finished, wait for the worker to drain all
  /// remaining items, and join the worker thread. Idempotent.
  void Drain() {
    queue_->MarkFinished();

    if (worker_.joinable()) {
      worker_.join();
    }
  }

 private:
  void WorkerLoop() {
    while (true) {
      queue_->WaitUntilNonEmptyOrFinished();

      // Fire the callback for each available item.
      // The callback pops from the queue — that is the established contract.
      while (queue_->Size() > 0) {
        if (request_.IsCancellationRequested() && queue_->Size() > kMaxCancellationDrainItems) {
          DropPendingItems();
          break;
        }

        SetCallbackInProgress(true);

        try {
          if (fn_(data_, user_data_) != 0) {
            request_.Cancel(Request::CancellationReason::StreamingCallback);
          }
        } catch (const std::exception& e) {
          logger_.Log(LogLevel::Warning,
                      fmt::format("streaming callback threw an exception; cancelling request: {}",
                                  e.what()));
          DisableAfterException(e.what());
          SetCallbackInProgress(false);
          return;
        } catch (...) {
          logger_.Log(LogLevel::Warning,
                      "streaming callback threw a non-std exception; cancelling request");
          DisableAfterException("non-standard exception");
          SetCallbackInProgress(false);
          return;
        }

        SetCallbackInProgress(false);
      }

      // Exit once the queue is finished and fully drained.
      if (queue_->IsFinished()) {
        return;
      }
    }
  }

  /// Called from the worker thread after the user callback throws. Marks the request
  /// cancelled (so PushItem becomes a no-op and the generator loop stops feeding work)
  /// and drops any items still queued so the destructor can join cleanly.
  void DisableAfterException(std::string_view detail) {
    request_.CancelFromStreamingCallbackException(detail);
    DropPendingItems();
  }

  void DropPendingItems() {
    while (queue_->TryPop()) {
    }
  }

  void SetCallbackInProgress(bool value) {
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      callback_in_progress_ = value;
    }

    if (!value) {
      callback_cv_.notify_all();
    }
  }

  const Request& request_;
  CallbackFn fn_;
  void* user_data_;
  ILogger& logger_;
  flStreamingCallbackData data_{};
  std::unique_ptr<ItemQueue> queue_;

  std::mutex callback_mutex_;
  std::condition_variable callback_cv_;
  bool callback_in_progress_ = false;
  std::thread worker_;
};

}  // namespace fl
