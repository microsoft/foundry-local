// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/types.h"
#include "items/item.h"
#include "util/key_value_pairs.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

/// Well-known request option: a leading system message applied to this turn's prompt without becoming conversation
/// history.
///
/// Chat sessions use it for request-scoped system instructions (the Responses API's `instructions`). Keeping it out
/// of the message list is the point: it can neither accumulate a copy per turn nor be replayed from a stored
/// conversation, and the value the current request supplies always wins.
inline constexpr const char* kSystemPromptOption = "system_prompt";

/// Generic inference request — pure input data.
/// Items are stored as borrowed pointers. Owned items are kept alive in owned_items.
struct Request {
  enum class CancellationReason : uint8_t {
    None,
    Caller,
    StreamingCallback,
    StreamingCallbackException,
    SessionShutdown,
  };

  enum class State : uint8_t {
    Ready,
    Running,
    CanceledByCaller,
    CanceledByStreamingCallback,
    CanceledByStreamingCallbackException,
    CanceledBySessionShutdown,
    Completing,
    Completed,
  };

  std::vector<Item*> items;  // all items (borrowed pointers)
  KeyValuePairs options;
  /// Request-local definitions already validated by an HTTP adapter. Used only to carry Chat
  /// declarations across the asynchronous streaming boundary without parsing or registering twice.
  std::optional<std::vector<ToolDefinition>> prepared_tool_definitions;
  /// Set only by trusted JSON request converters, never from generic native options.
  std::optional<ForcedToolChoice> forced_tool_choice;
  /// Explicit request metadata descriptor, validated by the provider converter.
  std::optional<RawEnvelopeDescriptor> raw_envelope_descriptor;

  /// Start indices, into `items`, of the replay segments the producer knows about. Ascending, and empty means the
  /// whole list is one segment.
  ///
  /// A producer that reconstructs a stored conversation marks where each recorded turn's items begin. Consumers
  /// must not group items across a boundary, so two recorded turns can never collapse into one message. A producer
  /// with no boundary information — a caller resending a flat conversation — leaves this empty and gets
  /// adjacency-based grouping instead.
  ///
  /// An index may repeat or sit past the end of `items`: a recorded turn that contributed no items still separates
  /// the turns around it. Use BeginItemSegment rather than writing indices directly — it is what keeps them
  /// ascending and consistent with `items`.
  std::vector<size_t> item_segment_starts;

  Request() = default;

  Request(Request&& other) noexcept
      : items(std::move(other.items)),
        options(std::move(other.options)),
        prepared_tool_definitions(std::move(other.prepared_tool_definitions)),
        forced_tool_choice(std::move(other.forced_tool_choice)),
        raw_envelope_descriptor(std::move(other.raw_envelope_descriptor)),
        item_segment_starts(std::move(other.item_segment_starts)),
        state_(other.state_.load(std::memory_order_relaxed)),
        cancellation_detail_(std::move(other.cancellation_detail_)),
        owned_items(std::move(other.owned_items)) {}

  Request& operator=(Request&& other) noexcept {
    items = std::move(other.items);
    options = std::move(other.options);
    prepared_tool_definitions = std::move(other.prepared_tool_definitions);
    forced_tool_choice = std::move(other.forced_tool_choice);
    raw_envelope_descriptor = std::move(other.raw_envelope_descriptor);
    item_segment_starts = std::move(other.item_segment_starts);
    state_.store(other.state_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    cancellation_detail_ = std::move(other.cancellation_detail_);
    owned_items = std::move(other.owned_items);
    return *this;
  }

  Request(const Request&) = delete;
  Request& operator=(const Request&) = delete;

  /// Add a pre-allocated owned item.
  void AddOwnedItem(std::unique_ptr<Item> item) {
    items.push_back(item.get());
    owned_items.push_back(std::move(item));
  }

  /// Add a borrowed item (caller must keep it alive).
  void AddBorrowedItem(Item* item) {
    items.push_back(item);
  }

  /// Mark the next item added as the first of a new replay segment. Recording a segment that then adds no items is
  /// meaningful: the boundary still keeps the turns on either side of it apart.
  void BeginItemSegment() {
    item_segment_starts.push_back(items.size());
  }

  /// Atomically wins cancellation against terminal publication. Returns false after completion has won.
  bool Cancel(CancellationReason reason = CancellationReason::Caller) const noexcept {
    const auto canceled_state = CanceledState(reason);
    auto state = state_.load(std::memory_order_acquire);
    while (state == State::Ready || state == State::Running) {
      if (state_.compare_exchange_weak(state, canceled_state,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
        return true;
      }
    }

    return IsCanceledState(state);
  }

  bool CancelFromStreamingCallbackException(std::string_view detail) const noexcept {
    try {
      std::lock_guard<std::mutex> lock(cancellation_detail_mutex_);
      cancellation_detail_ = detail;
    } catch (...) {
      // Cancellation itself must remain reliable if preserving diagnostic text runs out of memory.
    }

    return Cancel(CancellationReason::StreamingCallbackException);
  }

  bool IsCancellationRequested() const noexcept {
    return IsCanceledState(state_.load(std::memory_order_acquire));
  }

  CancellationReason GetCancellationReason() const noexcept {
    return ReasonFromState(state_.load(std::memory_order_acquire));
  }

  std::string CancellationDetail() const {
    std::lock_guard<std::mutex> lock(cancellation_detail_mutex_);
    return cancellation_detail_;
  }

  /// Starts first-time processing or reuses a request whose previous operation completed.
  bool TryBegin() const noexcept {
    auto state = state_.load(std::memory_order_acquire);
    while (state == State::Ready || state == State::Completed) {
      if (state_.compare_exchange_weak(state, State::Running,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
        return true;
      }
    }

    return false;
  }

  /// Atomically claims the normal or error terminal boundary. Returns false when cancellation won first.
  bool TryComplete() const noexcept {
    auto expected = State::Running;
    if (state_.compare_exchange_strong(expected, State::Completing,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
      return true;
    }

    return expected == State::Completing;
  }

  /// Makes a claimed completion reusable after the outer request call has finished publishing its result.
  void PublishCompletion() const noexcept {
    state_.store(State::Completed, std::memory_order_release);
  }

  bool IsCompleted() const noexcept {
    return state_.load(std::memory_order_acquire) == State::Completed;
  }

 private:
  static State CanceledState(CancellationReason reason) noexcept {
    switch (reason) {
      case CancellationReason::StreamingCallback:
        return State::CanceledByStreamingCallback;
      case CancellationReason::StreamingCallbackException:
        return State::CanceledByStreamingCallbackException;
      case CancellationReason::SessionShutdown:
        return State::CanceledBySessionShutdown;
      case CancellationReason::None:
      case CancellationReason::Caller:
      default:
        return State::CanceledByCaller;
    }
  }

  static bool IsCanceledState(State state) noexcept {
    return state == State::CanceledByCaller ||
           state == State::CanceledByStreamingCallback ||
           state == State::CanceledByStreamingCallbackException ||
           state == State::CanceledBySessionShutdown;
  }

  static CancellationReason ReasonFromState(State state) noexcept {
    switch (state) {
      case State::CanceledByCaller:
        return CancellationReason::Caller;
      case State::CanceledByStreamingCallback:
        return CancellationReason::StreamingCallback;
      case State::CanceledByStreamingCallbackException:
        return CancellationReason::StreamingCallbackException;
      case State::CanceledBySessionShutdown:
        return CancellationReason::SessionShutdown;
      default:
        return CancellationReason::None;
    }
  }

  mutable std::atomic<State> state_{State::Ready};
  mutable std::mutex cancellation_detail_mutex_;
  mutable std::string cancellation_detail_;
  std::vector<std::unique_ptr<Item>> owned_items;  // owned items (lifetime)
};

}  // namespace fl
