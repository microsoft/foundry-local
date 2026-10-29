// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/host_lifetime.h"

#include <gtest/gtest.h>

#include <chrono>
#include <latch>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using fl::hosting::HostLifetime;
using State = HostLifetime::State;
using TimePoint = HostLifetime::TimePoint;
using namespace std::chrono_literals;

constexpr TimePoint kStart{};
constexpr auto kStartupDeadline = kStart + 10s;

static_assert(!std::is_copy_constructible_v<HostLifetime::WorkGuard>);
static_assert(!std::is_copy_assignable_v<HostLifetime::WorkGuard>);
static_assert(std::is_nothrow_move_constructible_v<HostLifetime::WorkGuard>);
static_assert(std::is_nothrow_move_assignable_v<HostLifetime::WorkGuard>);

class HostLifetimeTest : public ::testing::Test {
 protected:
  HostLifetime host_{kStartupDeadline};
};

TEST_F(HostLifetimeTest, InitialProbesPreserveStartupDeadlineAndRejectAllWork) {
  const auto initial = host_.Poll(kStart);
  EXPECT_EQ(initial.state, State::kAwaitingFirstOwner);
  EXPECT_EQ(initial.owner_count, 0u);
  EXPECT_EQ(initial.ipc_work_count, 0u);
  EXPECT_EQ(initial.http_work_count, 0u);
  EXPECT_EQ(initial.deadline, kStartupDeadline);
  EXPECT_FALSE(host_.TryAdmitIpcWork(1, kStart));
  EXPECT_FALSE(host_.TryAdmitHttpWork(kStart));
  EXPECT_FALSE(host_.RemoveOwner(1, kStart));
  EXPECT_EQ(host_.Poll(kStartupDeadline - 1ns).state, State::kAwaitingFirstOwner);
  EXPECT_EQ(host_.Poll(kStartupDeadline).state, State::kShutdownCommitted);
  EXPECT_FALSE(host_.AddOwner(1, kStartupDeadline + 1s));
}

TEST_F(HostLifetimeTest, FirstAttachmentAtStartupDeadlineIsRejectedWithoutPriorPoll) {
  EXPECT_FALSE(host_.AddOwner(1, kStartupDeadline));
  const auto snapshot = host_.Poll(kStartupDeadline);
  EXPECT_EQ(snapshot.state, State::kShutdownCommitted);
  EXPECT_EQ(snapshot.owner_count, 0u);
  EXPECT_FALSE(snapshot.deadline);
}

TEST_F(HostLifetimeTest, FirstAttachmentBeforeStartupDeadlineCancelsStartupRetirement) {
  ASSERT_TRUE(host_.AddOwner(1, kStartupDeadline - 1ns));
  const auto snapshot = host_.Poll(kStartupDeadline + 1h);
  EXPECT_EQ(snapshot.state, State::kAccepting);
  EXPECT_EQ(snapshot.owner_count, 1u);
  EXPECT_FALSE(snapshot.deadline);
}

TEST_F(HostLifetimeTest, DeadStarterRetiresOnlyAnUnusedStartupAndCannotRevokeAnAttachedOwner) {
  ASSERT_TRUE(host_.RetireUnusedStartup(kStart));
  EXPECT_EQ(host_.Poll(kStart).state, State::kShutdownCommitted);
  EXPECT_FALSE(host_.AddOwner(1, kStart));
  HostLifetime attached{kStartupDeadline};
  ASSERT_TRUE(attached.AddOwner(1, kStart));
  EXPECT_FALSE(attached.RetireUnusedStartup(kStart));
  EXPECT_EQ(attached.Poll(kStart).owner_count, 1u);
}

TEST_F(HostLifetimeTest, StarterDeathAndFirstOwnerAttachmentHaveExactlyOneWinner) {
  for (int attempt = 0; attempt < 40; ++attempt) {
    HostLifetime startup{kStartupDeadline};
    std::latch ready{2};
    bool attached = false;
    bool retired = false;
    std::thread owner([&] {
      ready.arrive_and_wait();
      attached = startup.AddOwner(1, kStart);
    });
    std::thread dead_starter([&] {
      ready.arrive_and_wait();
      retired = startup.RetireUnusedStartup(kStart);
    });
    owner.join();
    dead_starter.join();
    EXPECT_NE(attached, retired);
    EXPECT_EQ(startup.Poll(kStart).state, attached ? State::kAccepting : State::kShutdownCommitted);
  }
}

TEST_F(HostLifetimeTest, DuplicateAttachmentAndUnknownRemovalDoNotChangeOwnerReferences) {
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  ASSERT_TRUE(host_.AddOwner(2, kStart));
  EXPECT_EQ(host_.Poll(kStart).owner_count, 2u);
  EXPECT_FALSE(host_.RemoveOwner(3, kStart));
  EXPECT_TRUE(host_.RemoveOwner(1, kStart));
  EXPECT_FALSE(host_.RemoveOwner(1, kStart));

  const auto snapshot = host_.Poll(kStart);
  EXPECT_EQ(snapshot.state, State::kAccepting);
  EXPECT_EQ(snapshot.owner_count, 1u);
  EXPECT_FALSE(snapshot.deadline);
  EXPECT_FALSE(host_.TryAdmitIpcWork(1, kStart));
  EXPECT_TRUE(host_.TryAdmitIpcWork(2, kStart));
  EXPECT_TRUE(host_.TryAdmitHttpWork(kStart));
}

TEST_F(HostLifetimeTest, LastOwnerWithoutWorkStartsFixedGraceAndProbesDoNotExtendIt) {
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  ASSERT_TRUE(host_.RemoveOwner(1, kStart + 1s));
  const auto snapshot = host_.Poll(kStart + 2s);
  EXPECT_EQ(snapshot.state, State::kGrace);
  EXPECT_EQ(snapshot.owner_count, 0u);
  EXPECT_EQ(snapshot.deadline, kStart + 6s);
  EXPECT_FALSE(host_.TryAdmitIpcWork(1, kStart + 2s));
  EXPECT_FALSE(host_.TryAdmitHttpWork(kStart + 2s));
  EXPECT_FALSE(host_.RemoveOwner(1, kStart + 3s));
  EXPECT_FALSE(host_.RemoveOwner(2, kStart + 4s));
  EXPECT_EQ(host_.Poll(kStart + 6s - 1ns).state, State::kGrace);
  EXPECT_EQ(host_.Poll(kStart + 6s).state, State::kShutdownCommitted);
}

TEST_F(HostLifetimeTest, IpcRequiresAttachedOwnerWhileHttpHasNoOwnerIdentity) {
  ASSERT_TRUE(host_.AddOwner(0, kStart));
  EXPECT_FALSE(host_.TryAdmitIpcWork(9, kStart));
  auto ipc = host_.TryAdmitIpcWork(0, kStart);
  auto http = host_.TryAdmitHttpWork(kStart);
  ASSERT_TRUE(ipc);
  ASSERT_TRUE(http);
  EXPECT_EQ(ipc.Owner(), 0u);
  EXPECT_FALSE(http.Owner());

  const auto snapshot = host_.Poll(kStart);
  EXPECT_EQ(snapshot.ipc_work_count, 1u);
  EXPECT_EQ(snapshot.http_work_count, 1u);
}

TEST_F(HostLifetimeTest, WebServiceStopRequiresTheSoleOwnerAndNoHttpWork) {
  EXPECT_FALSE(host_.TryAdmitWebServiceStop(1, kStart));
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  EXPECT_FALSE(host_.TryAdmitWebServiceStop(2, kStart));
  ASSERT_TRUE(host_.AddOwner(2, kStart));
  EXPECT_FALSE(host_.TryAdmitWebServiceStop(1, kStart));
  ASSERT_TRUE(host_.RemoveOwner(2, kStart));
  auto http = host_.TryAdmitHttpWork(kStart);
  EXPECT_FALSE(host_.TryAdmitWebServiceStop(1, kStart));
  http.Complete(kStart);
  auto stop = host_.TryAdmitWebServiceStop(1, kStart);
  ASSERT_TRUE(stop);
  EXPECT_FALSE(host_.TryAdmitHttpWork(kStart));
  EXPECT_FALSE(host_.TryAdmitWebServiceStop(1, kStart));
  EXPECT_TRUE(host_.TryAdmitIpcWork(1, kStart));
  stop.Complete(kStart);
  EXPECT_TRUE(host_.TryAdmitHttpWork(kStart));
}

TEST_F(HostLifetimeTest, WebServiceStopPinsDrainAndKeepsHttpClosedUntilItsMovedGuardCompletes) {
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  auto stop = host_.TryAdmitWebServiceStop(1, kStart);
  ASSERT_TRUE(stop);
  auto transferred = std::move(stop);
  stop = {};
  ASSERT_TRUE(host_.AddOwner(2, kStart));
  EXPECT_FALSE(host_.TryAdmitHttpWork(kStart));
  EXPECT_TRUE(host_.TryAdmitIpcWork(2, kStart));
  ASSERT_TRUE(host_.RemoveOwner(1, kStart));
  ASSERT_TRUE(host_.RemoveOwner(2, kStart));
  EXPECT_EQ(host_.Poll(kStart + 1h).state, State::kDraining);
  transferred.Complete(kStart + 1h);
  EXPECT_EQ(host_.Poll(kStart + 1h).deadline, kStart + 1h + 5s);
  ASSERT_TRUE(host_.AddOwner(3, kStart + 1h));
  EXPECT_TRUE(host_.TryAdmitHttpWork(kStart + 1h));
}

TEST_F(HostLifetimeTest, RacingHttpAdmissionAndWebServiceStopCannotBothSucceed) {
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  for (int attempt = 0; attempt < 40; ++attempt) {
    std::latch ready(2);
    HostLifetime::WorkGuard http, stop;
    std::thread request([&] {
      ready.count_down();
      ready.wait();
      http = host_.TryAdmitHttpWork(kStart);
    });
    std::thread stopping([&] {
      ready.count_down();
      ready.wait();
      stop = host_.TryAdmitWebServiceStop(1, kStart);
    });
    request.join();
    stopping.join();
    EXPECT_NE(static_cast<bool>(http), static_cast<bool>(stop));
  }
}

TEST_F(HostLifetimeTest, OwnerDisconnectPreservesBothWorkKindsAndGraceWaitsForLastCompletion) {
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  auto ipc = host_.TryAdmitIpcWork(1, kStart);
  auto http = host_.TryAdmitHttpWork(kStart);
  ASSERT_TRUE(host_.RemoveOwner(1, kStart + 1s));
  const auto draining = host_.Poll(kStart + 1h);
  EXPECT_EQ(draining.state, State::kDraining);
  EXPECT_EQ(draining.owner_count, 0u);
  EXPECT_EQ(draining.ipc_work_count, 1u);
  EXPECT_EQ(draining.http_work_count, 1u);
  EXPECT_FALSE(draining.deadline);
  EXPECT_EQ(ipc.Owner(), 1u);
  EXPECT_FALSE(host_.TryAdmitIpcWork(1, kStart + 1h));
  EXPECT_FALSE(host_.TryAdmitHttpWork(kStart + 1h));

  ipc.Complete(kStart + 1h);
  EXPECT_EQ(host_.Poll(kStart + 2h).state, State::kDraining);
  http.Complete(kStart + 2h);
  const auto grace = host_.Poll(kStart + 2h);
  EXPECT_EQ(grace.state, State::kGrace);
  EXPECT_EQ(grace.ipc_work_count, 0u);
  EXPECT_EQ(grace.http_work_count, 0u);
  EXPECT_EQ(grace.deadline, kStart + 2h + 5s);
  EXPECT_EQ(host_.Poll(kStart + 2h + 5s).state, State::kShutdownCommitted);
}

TEST_F(HostLifetimeTest, CompletingWorkWhileOwnerRemainsDoesNotStartGrace) {
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  auto work = host_.TryAdmitHttpWork(kStart);
  work.Complete(kStart + 1s);
  const auto snapshot = host_.Poll(kStart + 1h);
  EXPECT_EQ(snapshot.state, State::kAccepting);
  EXPECT_EQ(snapshot.http_work_count, 0u);
  EXPECT_FALSE(snapshot.deadline);
  ASSERT_TRUE(host_.RemoveOwner(1, kStart + 1h));
  EXPECT_EQ(host_.Poll(kStart + 1h).deadline, kStart + 1h + 5s);
}

TEST_F(HostLifetimeTest, ReattachmentDuringDrainAllowsWorkWithoutRebindingOldIpcOwner) {
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  auto old_work = host_.TryAdmitIpcWork(1, kStart);
  ASSERT_TRUE(host_.RemoveOwner(1, kStart + 1s));
  ASSERT_TRUE(host_.AddOwner(2, kStart + 1h));
  const auto snapshot = host_.Poll(kStart + 1h);
  EXPECT_EQ(snapshot.state, State::kAccepting);
  EXPECT_EQ(snapshot.ipc_work_count, 1u);
  EXPECT_EQ(old_work.Owner(), 1u);
  EXPECT_FALSE(snapshot.deadline);
  EXPECT_FALSE(host_.TryAdmitIpcWork(1, kStart + 1h));
  EXPECT_TRUE(host_.TryAdmitIpcWork(2, kStart + 1h));
  EXPECT_TRUE(host_.TryAdmitHttpWork(kStart + 1h));
  old_work.Complete(kStart + 1h);
  EXPECT_EQ(host_.Poll(kStart + 1h).state, State::kAccepting);
}

TEST_F(HostLifetimeTest, ReattachmentJustBeforeGraceDeadlineRestartsAcceptingAndNextGraceIsFresh) {
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  ASSERT_TRUE(host_.RemoveOwner(1, kStart));
  ASSERT_TRUE(host_.AddOwner(2, kStart + 5s - 1ns));
  EXPECT_EQ(host_.Poll(kStart + 6s).state, State::kAccepting);
  ASSERT_TRUE(host_.RemoveOwner(2, kStart + 7s));
  EXPECT_EQ(host_.Poll(kStart + 7s).deadline, kStart + 12s);
}

TEST_F(HostLifetimeTest, ReattachmentAtGraceDeadlineCommitsEvenWithoutPriorPoll) {
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  ASSERT_TRUE(host_.RemoveOwner(1, kStart));
  EXPECT_FALSE(host_.AddOwner(2, kStart + 5s));
  const auto snapshot = host_.Poll(kStart + 5s);
  EXPECT_EQ(snapshot.state, State::kShutdownCommitted);
  EXPECT_EQ(snapshot.owner_count, 0u);
  EXPECT_FALSE(snapshot.deadline);
}

TEST_F(HostLifetimeTest, ReattachmentAfterGraceDeadlineIsRejectedAndCommitmentCannotBeReversed) {
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  ASSERT_TRUE(host_.RemoveOwner(1, kStart));
  EXPECT_FALSE(host_.AddOwner(2, kStart + 6s));
  EXPECT_FALSE(host_.AddOwner(3, kStart + 1s));
  EXPECT_FALSE(host_.TryAdmitIpcWork(3, kStart + 1h));
  EXPECT_FALSE(host_.TryAdmitHttpWork(kStart + 1h));
  EXPECT_FALSE(host_.RemoveOwner(1, kStart + 1h));
  EXPECT_EQ(host_.Poll(kStart + 1h).state, State::kShutdownCommitted);
}

TEST_F(HostLifetimeTest, ExceptionUnwindingReleasesWorkAndNextObservationStartsConservativeGrace) {
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  EXPECT_THROW(
      {
        auto work = host_.TryAdmitHttpWork(kStart);
        ASSERT_TRUE(work);
        ASSERT_TRUE(host_.RemoveOwner(1, kStart + 1s));
        throw std::runtime_error("worker failed");
      },
      std::runtime_error);

  const auto snapshot = host_.Poll(kStart + 20s);
  EXPECT_EQ(snapshot.state, State::kGrace);
  EXPECT_EQ(snapshot.http_work_count, 0u);
  EXPECT_EQ(snapshot.deadline, kStart + 25s);
  EXPECT_EQ(host_.Poll(kStart + 25s).state, State::kShutdownCommitted);
}

TEST_F(HostLifetimeTest, MovedAndExplicitlyCompletedGuardsReleaseExactlyOnce) {
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  auto original = host_.TryAdmitIpcWork(1, kStart);
  auto moved = std::move(original);
  EXPECT_FALSE(original);
  EXPECT_FALSE(original.Owner());
  ASSERT_TRUE(moved);
  EXPECT_EQ(moved.Owner(), 1u);
  original.Complete(kStart + 1s);
  EXPECT_EQ(host_.Poll(kStart + 1s).ipc_work_count, 1u);
  ASSERT_TRUE(host_.RemoveOwner(1, kStart + 1s));
  moved.Complete(kStart + 2s);
  moved.Complete(kStart + 10s);
  EXPECT_FALSE(moved);
  EXPECT_FALSE(moved.Owner());
  const auto snapshot = host_.Poll(kStart + 2s);
  EXPECT_EQ(snapshot.ipc_work_count, 0u);
  EXPECT_EQ(snapshot.deadline, kStart + 7s);
}

TEST_F(HostLifetimeTest, MoveAssignmentReleasesReplacedWorkAndPreservesIncomingIdentity) {
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  auto ipc = host_.TryAdmitIpcWork(1, kStart);
  auto http = host_.TryAdmitHttpWork(kStart);
  http = std::move(ipc);
  EXPECT_FALSE(ipc);
  ASSERT_TRUE(http);
  EXPECT_EQ(http.Owner(), 1u);
  const auto snapshot = host_.Poll(kStart);
  EXPECT_EQ(snapshot.ipc_work_count, 1u);
  EXPECT_EQ(snapshot.http_work_count, 0u);

  http = HostLifetime::WorkGuard{};
  EXPECT_FALSE(http);
  EXPECT_EQ(host_.Poll(kStart).ipc_work_count, 0u);
}

TEST_F(HostLifetimeTest, GuardMoveAssignmentBetweenControllersDrainsOnlyTheReplacedAdmission) {
  HostLifetime other{kStartupDeadline};
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  ASSERT_TRUE(other.AddOwner(2, kStart));
  auto first = host_.TryAdmitHttpWork(kStart);
  auto second = other.TryAdmitIpcWork(2, kStart);
  ASSERT_TRUE(host_.RemoveOwner(1, kStart));
  first = std::move(second);
  EXPECT_EQ(host_.Poll(kStart + 1s).deadline, kStart + 6s);
  EXPECT_EQ(other.Poll(kStart + 1s).ipc_work_count, 1u);
  first.Complete(kStart + 2s);
  EXPECT_EQ(other.Poll(kStart + 2s).ipc_work_count, 0u);
}

TEST_F(HostLifetimeTest, EmptyGuardCompletionIsHarmless) {
  HostLifetime::WorkGuard empty;
  EXPECT_FALSE(empty);
  EXPECT_FALSE(empty.Owner());
  empty.Complete(kStart);
  EXPECT_EQ(host_.Poll(kStart).state, State::kAwaitingFirstOwner);
}

TEST(HostLifetimeGuardTest, AdmittedGuardsCanCompleteOrDestructAfterControllerDestruction) {
  auto host = std::make_unique<HostLifetime>(kStartupDeadline);
  ASSERT_TRUE(host->AddOwner(1, kStart));
  auto ipc = host->TryAdmitIpcWork(1, kStart);
  auto http = host->TryAdmitHttpWork(kStart);
  ASSERT_TRUE(ipc);
  ASSERT_TRUE(http);
  host.reset();
  EXPECT_EQ(ipc.Owner(), 1u);
  ipc.Complete(kStart + 1s);
  EXPECT_FALSE(ipc);
  EXPECT_TRUE(http);
}

TEST_F(HostLifetimeTest, OutOfOrderTimeSamplesCannotBackdateGrace) {
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  auto work = host_.TryAdmitHttpWork(kStart);
  ASSERT_TRUE(host_.RemoveOwner(1, kStart + 1s));
  EXPECT_EQ(host_.Poll(kStart + 10s).state, State::kDraining);
  work.Complete(kStart + 2s);
  EXPECT_EQ(host_.Poll(kStart + 3s).deadline, kStart + 15s);
}

TEST(HostLifetimeBoundaryTest, GraceDeadlineSaturatesInsteadOfOverflowing) {
  HostLifetime host{TimePoint::max()};
  const auto now = TimePoint::max() - 1s;
  ASSERT_TRUE(host.AddOwner(1, now));
  ASSERT_TRUE(host.RemoveOwner(1, now));
  EXPECT_EQ(host.Poll(now).deadline, TimePoint::max());
  EXPECT_EQ(host.Poll(TimePoint::max()).state, State::kShutdownCommitted);
}

TEST_F(HostLifetimeTest, ConcurrentOwnerRemovalAndWorkerCompletionDrainWithoutDestroyingWorkerAdmission) {
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  auto work = host_.TryAdmitIpcWork(1, kStart);
  ASSERT_TRUE(work);
  std::latch owner_removed{1};
  std::latch allow_completion{1};
  std::thread disconnect([&] {
    EXPECT_TRUE(host_.RemoveOwner(1, kStart + 1s));
    owner_removed.count_down();
  });
  std::thread worker([&, admitted = std::move(work)]() mutable {
    owner_removed.wait();
    EXPECT_EQ(admitted.Owner(), 1u);
    EXPECT_FALSE(host_.TryAdmitIpcWork(1, kStart + 1s));
    allow_completion.wait();
    admitted.Complete(kStart + 2s);
  });

  owner_removed.wait();
  const auto draining = host_.Poll(kStart + 1s);
  EXPECT_EQ(draining.state, State::kDraining);
  EXPECT_EQ(draining.ipc_work_count, 1u);
  EXPECT_FALSE(draining.deadline);
  allow_completion.count_down();
  disconnect.join();
  worker.join();
  const auto grace = host_.Poll(kStart + 2s);
  EXPECT_EQ(grace.state, State::kGrace);
  EXPECT_EQ(grace.ipc_work_count, 0u);
  EXPECT_EQ(grace.deadline, kStart + 7s);
}

TEST_F(HostLifetimeTest, ConcurrentDeadlinePollAndAttachmentCannotReviveCommittedHost) {
  ASSERT_TRUE(host_.AddOwner(1, kStart));
  ASSERT_TRUE(host_.RemoveOwner(1, kStart));
  std::latch start{1};
  bool attached = true;
  State observed = State::kAccepting;
  std::thread attach([&] {
    start.wait();
    attached = host_.AddOwner(2, kStart + 5s);
  });
  std::thread retire([&] {
    start.wait();
    observed = host_.Poll(kStart + 5s).state;
  });

  start.count_down();
  attach.join();
  retire.join();
  EXPECT_FALSE(attached);
  EXPECT_EQ(observed, State::kShutdownCommitted);
  EXPECT_EQ(host_.Poll(kStart + 5s).owner_count, 0u);
}

}  // namespace
