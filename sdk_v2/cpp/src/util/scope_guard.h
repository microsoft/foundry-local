// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <type_traits>
#include <utility>

namespace fl {

/// Runs an action when the enclosing scope exits, unless it has been dismissed.
///
/// The action must be `noexcept`. A guard exists to restore an invariant on paths where an exception may already be
/// in flight, so a throwing cleanup would terminate the process during unwinding. Requiring `noexcept` statically
/// keeps that contract at the call site rather than swallowing errors inside the destructor, which would hide
/// product failures on ordinary execution paths.
template <typename Action>
class ScopeGuard {
  static_assert(std::is_nothrow_invocable_v<Action&>,
                "ScopeGuard actions must be noexcept: cleanup can run while an exception is unwinding");

 public:
  explicit ScopeGuard(Action action) noexcept(std::is_nothrow_move_constructible_v<Action>)
      : action_(std::move(action)) {}

  ~ScopeGuard() {
    if (active_) {
      action_();
    }
  }

  ScopeGuard(const ScopeGuard&) = delete;
  ScopeGuard& operator=(const ScopeGuard&) = delete;
  ScopeGuard(ScopeGuard&&) = delete;
  ScopeGuard& operator=(ScopeGuard&&) = delete;

  /// Cancel the action — whatever it was protecting has been established successfully.
  void Dismiss() noexcept { active_ = false; }

 private:
  Action action_;
  bool active_ = true;
};

template <typename Action>
ScopeGuard(Action) -> ScopeGuard<Action>;

}  // namespace fl
