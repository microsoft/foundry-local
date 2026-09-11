// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for ScopeGuard — cleanup that must run on early exits, including during exception unwinding.

#include "util/scope_guard.h"

#include <gtest/gtest.h>

#include <stdexcept>

using namespace fl;

TEST(ScopeGuardTest, ActionRunsOnScopeExit) {
  int runs = 0;

  {
    ScopeGuard guard([&]() noexcept { ++runs; });
    EXPECT_EQ(runs, 0);
  }

  EXPECT_EQ(runs, 1);
}

TEST(ScopeGuardTest, DismissedActionDoesNotRun) {
  int runs = 0;

  {
    ScopeGuard guard([&]() noexcept { ++runs; });
    guard.Dismiss();
  }

  EXPECT_EQ(runs, 0);
}

TEST(ScopeGuardTest, ActionRunsWhileAnExceptionUnwinds) {
  int runs = 0;

  try {
    ScopeGuard guard([&]() noexcept { ++runs; });
    throw std::runtime_error("boom");
  } catch (const std::runtime_error&) {
    // The guard must have restored the invariant on the way out.
  }

  EXPECT_EQ(runs, 1);
}

TEST(ScopeGuardTest, DismissIsIdempotent) {
  int runs = 0;

  {
    ScopeGuard guard([&]() noexcept { ++runs; });
    guard.Dismiss();
    guard.Dismiss();
  }

  EXPECT_EQ(runs, 0);
}

TEST(ScopeGuardTest, GuardsRunInReverseScopeOrder) {
  std::vector<int> order;

  {
    ScopeGuard first([&]() noexcept { order.push_back(1); });
    ScopeGuard second([&]() noexcept { order.push_back(2); });
  }

  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], 2);
  EXPECT_EQ(order[1], 1);
}

TEST(ScopeGuardTest, CleanupRunsWhenTheGuardedOperationThrows) {
  // Mirrors ChatSession::UndoTurns: the transcript is already truncated when the generator is rewound, so a failing
  // rewind must invalidate the cached generator before the failure propagates rather than leaving the two diverged.
  bool invalidated = false;
  auto rewind = [] { throw std::runtime_error("failed to rewind generator"); };

  EXPECT_THROW(
      {
        ScopeGuard invalidate_on_failed_rewind([&]() noexcept { invalidated = true; });
        rewind();
        invalidate_on_failed_rewind.Dismiss();
      },
      std::runtime_error);

  EXPECT_TRUE(invalidated);
}

TEST(ScopeGuardTest, CleanupIsSkippedWhenTheGuardedOperationSucceeds) {
  bool invalidated = false;

  {
    ScopeGuard invalidate_on_failed_rewind([&]() noexcept { invalidated = true; });
    // Rewind succeeds.
    invalidate_on_failed_rewind.Dismiss();
  }

  EXPECT_FALSE(invalidated);
}
