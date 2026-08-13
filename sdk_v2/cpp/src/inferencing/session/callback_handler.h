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

#include "inferencing/session/operation_context.h"
#include "inferencing/session/request.h"
#include "items/item_queue.h"
#include "logger.h"

namespace fl {

/// Per-request streaming callback handler.
///
/// Created by SessionRuntime::CreateCallbackHandler() at the start of ProcessRequestImpl and destroyed (via
/// unique_ptr) when ProcessRequestImpl returns — which is strictly inside the lifetime of the
/// OperationContext it binds, so the worker thread can never outlive the state it stops.
///
/// Items are pushed to the ItemQueue on the caller's thread (preserving generation order). A single worker
/// thread pops items and fires the user callback. This decouples token generation from callback speed while
/// guaranteeing delivery order.
///
/// Cancellation: a callback returning non-zero (or throwing) stops **this exact operation** with
/// StopReason::kCallbackStop. That reaches the engine like any other stop, but Session::ProcessRequest maps
/// it to a normal completion with FOUNDRY_LOCAL_FINISH_NONE, preserving the established streaming/SSE
/// semantics where a client hanging up is not an error.
///
/// Quiesce() is the seal's precondition: it waits until everything pushed so far has been delivered and no
/// callback invocation is in flight, *without* finishing the queue or joining the worker, so the caller can
/// still push a final chunk after the outcome is committed. Destruction drains the queue and joins (RAII).
struct CallbackHandler {
  using CallbackFn = std::function<int(flStreamingCallbackData, void*)>;

  /// Production constructor: bound to the operation driving the run.
  CallbackHandler(const OperationContext& operation, CallbackFn callback_fn, ILogger& logger,
                  void* user_data = nullptr)
      : operation_(&operation), fn_(std::move(callback_fn)), user_data_(user_data), logger_(logger) {
    Start();
  }

  /// Request-only constructor for direct-Request coverage that has no operation (no runtime run). It
  /// latches the Request's diagnostic flag instead of stopping an operation, and is never used in
  /// production — every production handler is created through SessionRuntime::CreateCallbackHandler.
  CallbackHandler(const Request& request, CallbackFn callback_fn, ILogger& logger, void* user_data = nullptr)
      : request_control_(request.Control()),
        fn_(std::move(callback_fn)),
        user_data_(user_data),
        logger_(logger) {
    assert(request_control_ && "Request-only CallbackHandler requires a usable Request");
    Start();
  }

  ~CallbackHandler() {
    Drain();
  }

  CallbackHandler(const CallbackHandler&) = delete;
  CallbackHandler& operator=(const CallbackHandler&) = delete;

  /// Push an item into the queue and wake the worker.
  /// Called from the generator thread — returns immediately.
  void PushItem(std::unique_ptr<Item> item) {
    if (Stopped()) {
      return;
    }

    queue_->Push(std::move(item));
  }

  /// Publish the already-prepared terminal envelope after the operation outcome is sealed.
  ///
  /// Unlike PushItem(), this deliberately bypasses the stop filter so a cancelled operation can still tell a
  /// streaming consumer its truthful terminal finish reason. The item must be fully allocated before sealing.
  /// Queue growth is the only remaining throwing step; terminal delivery is best-effort and cannot reopen a
  /// sealed outcome, so allocation failure is contained here without logging or changing operation state.
  void PushFinalItem(std::unique_ptr<Item> item) noexcept {
    try {
      queue_->Push(std::move(item));
    } catch (...) {
    }
  }

  /// Wait until every item pushed so far has been processed and no callback invocation is in flight.
  ///
  /// Deliberately does *not* mark the queue finished and does *not* join: the caller may still need to push
  /// a final chunk once it has committed its outcome. The user callback is never invoked while the idle
  /// mutex is held, so a callback that pushes or inspects state cannot deadlock against this wait.
  ///
  /// This is what makes a completion seal meaningful: after Quiesce() returns, no callback decision is
  /// pending, so TrySeal() sees the final answer instead of racing a worker that is about to stop the run.
  void Quiesce() {
    std::unique_lock<std::mutex> lock(idle_mu_);
    idle_cv_.wait(lock, [this] { return worker_done_ || (!in_callback_ && queue_->Size() == 0); });
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
  void Start() {
    assert(fn_ && "Streaming callback cannot be null");
    data_.version = FOUNDRY_LOCAL_API_VERSION;
    data_.item_queue = queue_->AsApiType();
    worker_ = std::thread(&CallbackHandler::WorkerLoop, this);
  }

  bool Stopped() const {
    if (operation_ != nullptr) {
      return operation_->ShouldStop();
    }

    // Only a moved-from Request lacks a control. The constructor asserts in debug builds; treating that
    // invalid test-only shell as stopped keeps release builds from dereferencing null.
    return !request_control_ ||
           request_control_->Diagnostic(DiagnosticBit::kCancelled, std::memory_order_relaxed);
  }

  /// Ask the bound operation to stop because the consumer no longer wants the stream.
  void RequestCallbackStop() {
    if (operation_ != nullptr) {
      operation_->RequestStop(StopReason::kCallbackStop);
      return;
    }

    // Diagnostic-only fallback for the operation-less constructor. Uses the mirror write so it cannot
    // recurse back through the cancel routing.
    if (request_control_) {
      request_control_->SetDiagnostic(DiagnosticBit::kCancelled, true, std::memory_order_relaxed);
    }
  }

  void BeginCallback() {
    std::lock_guard<std::mutex> lock(idle_mu_);
    in_callback_ = true;
  }

  /// Publish "no callback in flight". Every caller latches the callback's stop decision *before* this, so a
  /// Quiesce() that wakes here can never miss a stop the worker had already decided on.
  void EndCallback() {
    {
      std::lock_guard<std::mutex> lock(idle_mu_);
      in_callback_ = false;
    }

    idle_cv_.notify_all();
  }

  void MarkWorkerDone() {
    {
      std::lock_guard<std::mutex> lock(idle_mu_);
      in_callback_ = false;
      worker_done_ = true;
    }

    idle_cv_.notify_all();
  }

  void WorkerLoop() {
    while (true) {
      queue_->WaitUntilNonEmptyOrFinished();

      // Fire the callback for each available item.
      // The callback pops from the queue — that is the established contract.
      while (queue_->Size() > 0) {
        BeginCallback();

        try {
          if (fn_(data_, user_data_) != 0) {
            // Latch before publishing idle so Quiesce() cannot observe a quiet worker whose stop decision
            // has not landed yet.
            RequestCallbackStop();
          }
        } catch (const std::exception& e) {
          DisableAfterException();
          MarkWorkerDone();
          logger_.Log(LogLevel::Warning,
                      fmt::format("streaming callback threw an exception; cancelling request: {}", e.what()));
          return;
        } catch (...) {
          DisableAfterException();
          MarkWorkerDone();
          logger_.Log(LogLevel::Warning, "streaming callback threw a non-std exception; cancelling request");
          return;
        }

        EndCallback();
      }

      // Exit once the queue is finished and fully drained.
      if (queue_->IsFinished()) {
        MarkWorkerDone();
        return;
      }
    }
  }

  /// Called from the worker thread after the user callback throws. Stops the operation (so PushItem becomes
  /// a no-op and the generation loop stops feeding work) and drops any items still queued so the destructor
  /// can join cleanly.
  void DisableAfterException() {
    RequestCallbackStop();

    while (queue_->TryPop()) {
    }
  }

  /// Production borrows the operation context, which outlives the drained worker. The request-only
  /// compatibility path retains its stable control instead of borrowing Request storage.
  const OperationContext* operation_ = nullptr;
  std::shared_ptr<RequestControl> request_control_;

  CallbackFn fn_;
  void* user_data_;
  ILogger& logger_;
  flStreamingCallbackData data_{};
  std::unique_ptr<ItemQueue> queue_ = std::make_unique<ItemQueue>();

  /// Idle accounting for Quiesce(). Never held while the user callback runs.
  mutable std::mutex idle_mu_;
  std::condition_variable idle_cv_;
  bool in_callback_ = false;
  bool worker_done_ = false;

  std::thread worker_;
};

}  // namespace fl
