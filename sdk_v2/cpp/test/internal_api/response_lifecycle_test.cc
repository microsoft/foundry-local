// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for the Responses lifecycle: model-bound continuation and the lease that keeps deletion coherent with
// requests that are already generating.

#include "inferencing/generative/openresponses/response_store.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace fl;
using json = nlohmann::json;

namespace {

/// Records what the store admits, standing in for the service's session cache.
class RecordingAdmission final : public IResponseAdmission {
 public:
  void Admit(const std::string& response_id) override { admitted.push_back(response_id); }

  std::vector<std::string> admitted;
};

/// Records what the store drops, standing in for SessionManager::EvictCached.
class RecordingCache final : public IResponseCacheCoordinator {
 public:
  void Drop(const std::string& response_id) noexcept override { dropped.push_back(response_id); }

  std::vector<std::string> dropped;
};

class ThrowingAdmission final : public IResponseAdmission {
 public:
  void Admit(const std::string&) override { throw std::runtime_error("cache admission failed"); }
};

/// A response object as the handler stores it: only `previous_response_id` and `output` matter to the store.
json ResponseJson(const std::string& id, const std::string& previous_id = "") {
  json response = {{"id", id}, {"output", json::array()}};
  if (!previous_id.empty()) {
    response["previous_response_id"] = previous_id;
  }

  return response;
}

/// Run one complete request through the store the way the handler does, and report whether it was published.
bool RunTurn(ResponseStore& store, const std::string& id, const std::string& previous_id,
             const std::string& model_id, IResponseAdmission* admission = nullptr) {
  auto continuation = store.BeginResponse(previous_id, model_id);
  EXPECT_EQ(continuation.status, ContinuationStatus::kOk);

  return store.Commit(continuation.lease,
                      ResponseStore::StoredResponse{id, model_id, ResponseJson(id, previous_id), json::array()},
                      admission);
}

}  // namespace

// ========================================================================
// Cross-model continuation
// ========================================================================

TEST(ResponseContinuationTest, ContinuingWithTheModelThatProducedTheParentIsAccepted) {
  ResponseStore store;
  store.Store("resp_1", ResponseJson("resp_1"), json::array(), "model-a");

  auto continuation = store.BeginResponse("resp_1", "model-a");

  EXPECT_EQ(continuation.status, ContinuationStatus::kOk);
  EXPECT_TRUE(static_cast<bool>(continuation.lease));
}

TEST(ResponseContinuationTest, ContinuingWithADifferentModelIsRejected) {
  ResponseStore store;
  store.Store("resp_1", ResponseJson("resp_1"), json::array(), "model-a");

  auto continuation = store.BeginResponse("resp_1", "model-b");

  EXPECT_EQ(continuation.status, ContinuationStatus::kModelMismatch);
  EXPECT_EQ(continuation.parent_model_id, "model-a");
  EXPECT_FALSE(static_cast<bool>(continuation.lease));
}

TEST(ResponseContinuationTest, ARejectedCrossModelContinuationHoldsNoLease) {
  ResponseStore store;
  store.Store("resp_1", ResponseJson("resp_1"), json::array(), "model-a");

  auto continuation = store.BeginResponse("resp_1", "model-b");

  EXPECT_EQ(store.InFlightResponses(), 0u);
}

TEST(ResponseContinuationTest, TheModelOfTheImmediateParentDecidesRegardlessOfChainDepth) {
  ResponseStore store;
  store.Store("resp_1", ResponseJson("resp_1"), json::array(), "model-a");
  store.Store("resp_2", ResponseJson("resp_2", "resp_1"), json::array(), "model-a");

  EXPECT_EQ(store.BeginResponse("resp_2", "model-b").status, ContinuationStatus::kModelMismatch);
  EXPECT_EQ(store.BeginResponse("resp_2", "model-a").status, ContinuationStatus::kOk);
}

TEST(ResponseContinuationTest, AnUnknownParentIsReportedAsAnUnavailableChainNotAModelMismatch) {
  ResponseStore store;

  auto continuation = store.BeginResponse("resp_missing", "model-a");

  EXPECT_EQ(continuation.status, ContinuationStatus::kChainUnavailable);
  EXPECT_EQ(store.InFlightResponses(), 0u);
}

TEST(ResponseContinuationTest, ABrokenChainIsReportedBeforeTheModelIsCompared) {
  ResponseStore store;
  // resp_2's parent was never stored, so the conversation cannot be replayed at all.
  store.Store("resp_2", ResponseJson("resp_2", "resp_1"), json::array(), "model-a");

  EXPECT_EQ(store.BeginResponse("resp_2", "model-b").status, ContinuationStatus::kChainUnavailable);
}

TEST(ResponseContinuationTest, AParentStoredWithoutAModelConstrainsNothing) {
  ResponseStore store;
  store.Store("resp_1", ResponseJson("resp_1"), json::array());

  EXPECT_EQ(store.BeginResponse("resp_1", "model-a").status, ContinuationStatus::kOk);
  EXPECT_EQ(store.BeginResponse("resp_1", "model-b").status, ContinuationStatus::kOk);
}

TEST(ResponseContinuationTest, AModelBoundParentDoesNotConstrainACallerThatResolvesNoModel) {
  ResponseStore store;
  store.Store("resp_1", ResponseJson("resp_1"), json::array(), "model-a");

  EXPECT_EQ(store.BeginResponse("resp_1", "").status, ContinuationStatus::kOk);
}

TEST(ResponseContinuationTest, ANewConversationIsAlwaysAccepted) {
  ResponseStore store;

  auto continuation = store.BeginResponse("", "model-a");

  EXPECT_EQ(continuation.status, ContinuationStatus::kOk);
  EXPECT_TRUE(static_cast<bool>(continuation.lease));
}

TEST(ResponseContinuationTest, CommittedResponsesCarryTheirModelToTheNextTurn) {
  ResponseStore store;
  ASSERT_TRUE(RunTurn(store, "resp_1", "", "model-a"));
  ASSERT_TRUE(RunTurn(store, "resp_2", "resp_1", "model-a"));

  EXPECT_EQ(store.BeginResponse("resp_2", "model-a").status, ContinuationStatus::kOk);
  EXPECT_EQ(store.BeginResponse("resp_2", "model-b").status, ContinuationStatus::kModelMismatch);
}

TEST(ResponseContinuationTest, AcceptingAContinuationKeepsTheWholeChainResident) {
  // Capacity 3 with a 3-hop conversation: opening a continuation must refresh every hop, otherwise storing the
  // fourth response evicts the root and the conversation can no longer be rebuilt.
  ResponseStore store(3);
  store.Store("resp_1", ResponseJson("resp_1"), json::array(), "model-a");
  store.Store("resp_2", ResponseJson("resp_2", "resp_1"), json::array(), "model-a");
  store.Store("resp_3", ResponseJson("resp_3", "resp_2"), json::array(), "model-a");

  auto continuation = store.BeginResponse("resp_3", "model-a");
  ASSERT_EQ(continuation.status, ContinuationStatus::kOk);

  EXPECT_TRUE(store.Get("resp_1").has_value());
  EXPECT_TRUE(store.Get("resp_3").has_value());
}

TEST(ResponseContinuationTest, CapacityEvictedParentCommitsFromLeaseSnapshotAndReconstructsExactly) {
  ResponseStore store(1);
  const auto parent_input = json::array(
      {{{"type", "message"}, {"role", "user"}, {"content", "parent input"}}});
  const auto parent_output = json::array(
      {{{"type", "message"}, {"role", "assistant"}, {"content", "parent output"}}});
  auto parent_response = ResponseJson("parent");
  parent_response["output"] = parent_output;
  store.Store("parent", std::move(parent_response), parent_input, "model-a");

  auto continuation = store.BeginResponse("parent", "model-a");
  ASSERT_EQ(continuation.status, ContinuationStatus::kOk);

  store.Store("unrelated", ResponseJson("unrelated"), json::array(), "model-a");
  ASSERT_FALSE(store.Get("parent").has_value());

  const auto child_input = json::array(
      {{{"type", "message"}, {"role", "user"}, {"content", "child input"}}});
  const auto child_output = json::array(
      {{{"type", "message"}, {"role", "assistant"}, {"content", "child output"}}});
  auto child_response = ResponseJson("child", "parent");
  child_response["output"] = child_output;
  ASSERT_TRUE(store.Commit(
      continuation.lease,
      ResponseStore::StoredResponse{"child", "model-a", std::move(child_response), child_input},
      nullptr));

  const auto context = store.BuildChainContext("child");
  ASSERT_TRUE(context.has_value());
  ASSERT_EQ(context->size(), 2u);
  EXPECT_EQ((*context)[0].input_items, parent_input);
  EXPECT_EQ((*context)[0].output_items, parent_output);
  EXPECT_EQ((*context)[1].input_items, child_input);
  EXPECT_EQ((*context)[1].output_items, child_output);
}

TEST(ResponseContinuationTest, NormalCompactionAndLeaseSnapshotDoNotDuplicateReplayHops) {
  ResponseStore store(2);
  store.Store("root", ResponseJson("root"), json::array({{{"content", "root"}}}), "model-a");
  store.Store("parent", ResponseJson("parent", "root"), json::array({{{"content", "parent"}}}), "model-a");

  auto continuation = store.BeginResponse("parent", "model-a");
  ASSERT_EQ(continuation.status, ContinuationStatus::kOk);

  // This compacts root into parent. Committing child then normally compacts parent into child; the lease snapshot must
  // not be layered over either resident prefix.
  store.Store("unrelated", ResponseJson("unrelated"), json::array(), "model-a");
  ASSERT_TRUE(store.Commit(
      continuation.lease,
      ResponseStore::StoredResponse{
          "child", "model-a", ResponseJson("child", "parent"), json::array({{{"content", "child"}}})},
      nullptr));

  const auto context = store.BuildChainContext("child");
  ASSERT_TRUE(context.has_value());
  ASSERT_EQ(context->size(), 3u);
  EXPECT_EQ((*context)[0].input_items[0]["content"], "root");
  EXPECT_EQ((*context)[1].input_items[0]["content"], "parent");
  EXPECT_EQ((*context)[2].input_items[0]["content"], "child");
}

// ========================================================================
// Lease lifetime
// ========================================================================

TEST(ResponseLeaseTest, AnOpenLeaseIsReleasedWhenItGoesOutOfScope) {
  ResponseStore store;
  store.Store("resp_1", ResponseJson("resp_1"), json::array(), "model-a");

  {
    auto continuation = store.BeginResponse("resp_1", "model-a");
    EXPECT_EQ(store.InFlightResponses(), 1u);
  }

  EXPECT_EQ(store.InFlightResponses(), 0u);
}

TEST(ResponseLeaseTest, CommittingReleasesTheLease) {
  ResponseStore store;

  auto continuation = store.BeginResponse("", "model-a");
  ASSERT_TRUE(store.Commit(continuation.lease,
                           ResponseStore::StoredResponse{"resp_1", "model-a", ResponseJson("resp_1"), json::array()},
                           nullptr));

  EXPECT_EQ(store.InFlightResponses(), 0u);
  EXPECT_FALSE(static_cast<bool>(continuation.lease));
}

TEST(ResponseLeaseTest, MovingALeaseTransfersTheSingleRegistration) {
  ResponseStore store;

  auto continuation = store.BeginResponse("", "model-a");
  ResponseLease moved = std::move(continuation.lease);

  EXPECT_FALSE(static_cast<bool>(continuation.lease));
  EXPECT_TRUE(static_cast<bool>(moved));
  EXPECT_EQ(store.InFlightResponses(), 1u);

  moved.Release();
  EXPECT_EQ(store.InFlightResponses(), 0u);
}

TEST(ResponseLeaseTest, ReleaseIsIdempotent) {
  ResponseStore store;

  auto continuation = store.BeginResponse("", "model-a");
  continuation.lease.Release();
  continuation.lease.Release();

  EXPECT_EQ(store.InFlightResponses(), 0u);
}

TEST(ResponseLeaseTest, CommittingWithoutALeaseStoresNothing) {
  ResponseStore store;
  ResponseLease empty;
  RecordingAdmission admission;

  EXPECT_FALSE(store.Commit(
      empty, ResponseStore::StoredResponse{"resp_1", "model-a", ResponseJson("resp_1"), json::array()}, &admission));
  EXPECT_FALSE(store.Get("resp_1").has_value());
  EXPECT_TRUE(admission.admitted.empty());
}

TEST(ResponseLeaseTest, CommittingALeaseFromAnotherStoreLeavesThatLeaseIntact) {
  // Consuming a foreign lease would strand its owner's registration forever, which is exactly the bounded state the
  // lease is supposed to guarantee.
  ResponseStore owner;
  ResponseStore other;
  RecordingAdmission admission;

  auto continuation = owner.BeginResponse("", "model-a");
  ASSERT_EQ(owner.InFlightResponses(), 1u);

  EXPECT_FALSE(other.Commit(
      continuation.lease, ResponseStore::StoredResponse{"resp_1", "model-a", ResponseJson("resp_1"), json::array()},
      &admission));
  EXPECT_FALSE(other.Get("resp_1").has_value());
  EXPECT_TRUE(admission.admitted.empty());
  EXPECT_TRUE(static_cast<bool>(continuation.lease)) << "the foreign store consumed a lease it does not own";
  EXPECT_EQ(owner.InFlightResponses(), 1u);

  // Still usable by its owner, and still released normally.
  EXPECT_TRUE(owner.Commit(
      continuation.lease, ResponseStore::StoredResponse{"resp_1", "model-a", ResponseJson("resp_1"), json::array()},
      nullptr));
  EXPECT_EQ(owner.InFlightResponses(), 0u);
}

// ========================================================================
// Delete versus in-flight continuation
// ========================================================================

TEST(ResponseDeleteRaceTest, CommitAdmitsTheSessionExactlyOnceWhenNothingWasDeleted) {
  ResponseStore store;
  RecordingAdmission admission;

  EXPECT_TRUE(RunTurn(store, "resp_1", "", "model-a", &admission));

  EXPECT_EQ(admission.admitted, std::vector<std::string>{"resp_1"});
  EXPECT_TRUE(store.Get("resp_1").has_value());
}

TEST(ResponseDeleteRaceTest, AdmissionFailureLeavesResponseUnpublishedAndPropagates) {
  ResponseStore store;
  ThrowingAdmission admission;
  auto continuation = store.BeginResponse("", "model-a");

  EXPECT_THROW(
      store.Commit(
          continuation.lease,
          ResponseStore::StoredResponse{"resp_1", "model-a", ResponseJson("resp_1"), json::array()},
          &admission),
      std::runtime_error);

  EXPECT_FALSE(store.Get("resp_1").has_value());
  EXPECT_EQ(store.Size(), 0u);
  EXPECT_EQ(store.InFlightResponses(), 0u);
  EXPECT_FALSE(static_cast<bool>(continuation.lease));
}

TEST(ResponseDeleteRaceTest, MetadataPreparationFailuresPreserveReplacementAndSkipAdmission) {
  const std::vector<ResponseStore::StorePhase> phases = {
      ResponseStore::StorePhase::kAfterEntriesCloned,
      ResponseStore::StorePhase::kAfterIndexRebuilt,
      ResponseStore::StorePhase::kAfterEntryUpdated,
  };

  for (const auto phase : phases) {
    SCOPED_TRACE(static_cast<int>(phase));
    std::optional<ResponseStore::StorePhase> fault;
    ResponseStore store(ResponseStore::kDefaultCapacity, nullptr,
                        [&fault](ResponseStore::StorePhase current) {
                          if (fault == current) {
                            throw std::runtime_error("injected metadata failure");
                          }
                        });
    store.Store("resp_1", {{"id", "resp_1"}, {"version", "original"}}, json::array(), "model-a");
    store.Store("resp_2", {{"id", "resp_2"}, {"version", "untouched"}}, json::array(), "model-a");
    const auto order_before = store.List().data;

    auto continuation = store.BeginResponse("", "model-a");
    RecordingAdmission admission;
    fault = phase;

    EXPECT_THROW(
        store.Commit(
            continuation.lease,
            ResponseStore::StoredResponse{
                "resp_1", "model-a", {{"id", "resp_1"}, {"version", "replacement"}}, json::array()},
            &admission),
        std::runtime_error);

    EXPECT_TRUE(admission.admitted.empty());
    EXPECT_EQ(store.Size(), 2u);
    EXPECT_EQ(store.List().data, order_before);
    const auto original = store.Get("resp_1");
    ASSERT_TRUE(original.has_value());
    EXPECT_EQ((*original)["version"], "original");
    EXPECT_EQ(store.InFlightResponses(), 0u);
    EXPECT_FALSE(static_cast<bool>(continuation.lease));
  }
}

TEST(ResponseDeleteRaceTest, CompactionFailurePreservesEveryMetadataEntryAndSkipsAdmission) {
  std::optional<ResponseStore::StorePhase> fault;
  ResponseStore store(2, nullptr, [&fault](ResponseStore::StorePhase current) {
    if (fault == current) {
      throw std::runtime_error("injected compaction failure");
    }
  });
  store.Store("root", ResponseJson("root"), json::array({{{"content", "root"}}}), "model-a");
  store.Store("child", ResponseJson("child", "root"), json::array({{{"content", "child"}}}), "model-a");
  const auto order_before = store.List().data;
  const auto chain_before = store.BuildChainContext("child");
  ASSERT_TRUE(chain_before.has_value());

  auto continuation = store.BeginResponse("", "model-a");
  RecordingAdmission admission;
  fault = ResponseStore::StorePhase::kAfterCompaction;

  EXPECT_THROW(
      store.Commit(
          continuation.lease,
          ResponseStore::StoredResponse{"other", "model-a", ResponseJson("other"), json::array()},
          &admission),
      std::runtime_error);

  EXPECT_TRUE(admission.admitted.empty());
  EXPECT_EQ(store.Size(), 2u);
  EXPECT_EQ(store.List().data, order_before);
  EXPECT_FALSE(store.Get("other").has_value());
  EXPECT_TRUE(store.Get("root").has_value());
  const auto chain_after = store.BuildChainContext("child");
  ASSERT_TRUE(chain_after.has_value());
  ASSERT_EQ(chain_after->size(), chain_before->size());
  for (size_t i = 0; i < chain_before->size(); ++i) {
    EXPECT_EQ((*chain_after)[i].input_items, (*chain_before)[i].input_items);
    EXPECT_EQ((*chain_after)[i].output_items, (*chain_before)[i].output_items);
  }
  EXPECT_EQ(store.InFlightResponses(), 0u);
}

TEST(ResponseDeleteRaceTest, AdmissionFailurePreservesExistingMetadataExactly) {
  ResponseStore store;
  store.Store("resp_1", {{"id", "resp_1"}, {"version", "original"}}, json::array(), "model-a");
  store.Store("resp_2", {{"id", "resp_2"}, {"version", "untouched"}}, json::array(), "model-a");
  const auto order_before = store.List().data;

  auto continuation = store.BeginResponse("", "model-a");
  ThrowingAdmission admission;

  EXPECT_THROW(
      store.Commit(
          continuation.lease,
          ResponseStore::StoredResponse{
              "resp_1", "model-a", {{"id", "resp_1"}, {"version", "replacement"}}, json::array()},
          &admission),
      std::runtime_error);

  EXPECT_EQ(store.Size(), 2u);
  EXPECT_EQ(store.List().data, order_before);
  const auto original = store.Get("resp_1");
  ASSERT_TRUE(original.has_value());
  EXPECT_EQ((*original)["version"], "original");
  EXPECT_EQ(store.InFlightResponses(), 0u);
}

TEST(ResponseDeleteRaceTest, DeletingTheParentMidFlightRejectsTheChildAndCachesNothing) {
  ResponseStore store;
  RecordingAdmission admission;
  store.Store("resp_1", ResponseJson("resp_1"), json::array(), "model-a");

  auto continuation = store.BeginResponse("resp_1", "model-a");
  ASSERT_EQ(continuation.status, ContinuationStatus::kOk);

  // The DELETE lands while resp_2 is still generating.
  ASSERT_TRUE(store.Delete("resp_1"));

  const bool committed = store.Commit(
      continuation.lease,
      ResponseStore::StoredResponse{"resp_2", "model-a", ResponseJson("resp_2", "resp_1"), json::array()},
      &admission);

  EXPECT_FALSE(committed);
  EXPECT_FALSE(store.Get("resp_2").has_value());
  EXPECT_TRUE(admission.admitted.empty()) << "a session was cached under a response derived from deleted content";
  EXPECT_EQ(store.InFlightResponses(), 0u);
}

TEST(ResponseDeleteRaceTest, DeletingAGrandparentMidFlightAlsoRejectsTheChild) {
  ResponseStore store;
  RecordingAdmission admission;
  store.Store("resp_1", ResponseJson("resp_1"), json::array(), "model-a");
  store.Store("resp_2", ResponseJson("resp_2", "resp_1"), json::array(), "model-a");

  auto continuation = store.BeginResponse("resp_2", "model-a");
  ASSERT_EQ(continuation.status, ContinuationStatus::kOk);

  ASSERT_TRUE(store.Delete("resp_1"));

  EXPECT_FALSE(store.Commit(
      continuation.lease,
      ResponseStore::StoredResponse{"resp_3", "model-a", ResponseJson("resp_3", "resp_2"), json::array()},
      &admission));
  EXPECT_TRUE(admission.admitted.empty());
}

TEST(ResponseDeleteRaceTest, DeletingAnUnrelatedConversationLeavesTheInFlightRequestAlone) {
  ResponseStore store;
  RecordingAdmission admission;
  store.Store("resp_1", ResponseJson("resp_1"), json::array(), "model-a");
  store.Store("other_1", ResponseJson("other_1"), json::array(), "model-a");

  auto continuation = store.BeginResponse("resp_1", "model-a");
  ASSERT_EQ(continuation.status, ContinuationStatus::kOk);

  ASSERT_TRUE(store.Delete("other_1"));

  EXPECT_TRUE(store.Commit(
      continuation.lease,
      ResponseStore::StoredResponse{"resp_2", "model-a", ResponseJson("resp_2", "resp_1"), json::array()},
      &admission));
  EXPECT_EQ(admission.admitted, std::vector<std::string>{"resp_2"});
}

TEST(ResponseDeleteRaceTest, DeletingASiblingBranchLeavesTheInFlightRequestAlone) {
  ResponseStore store;
  RecordingAdmission admission;
  store.Store("resp_1", ResponseJson("resp_1"), json::array(), "model-a");
  store.Store("branch_a", ResponseJson("branch_a", "resp_1"), json::array(), "model-a");
  store.Store("branch_b", ResponseJson("branch_b", "resp_1"), json::array(), "model-a");

  auto continuation = store.BeginResponse("branch_a", "model-a");
  ASSERT_EQ(continuation.status, ContinuationStatus::kOk);

  // Deleting the other branch touches neither the shared root nor branch_a.
  ASSERT_TRUE(store.Delete("branch_b"));

  EXPECT_TRUE(store.Commit(
      continuation.lease,
      ResponseStore::StoredResponse{"resp_2", "model-a", ResponseJson("resp_2", "branch_a"), json::array()},
      &admission));
  EXPECT_EQ(admission.admitted, std::vector<std::string>{"resp_2"});
}

TEST(ResponseDeleteRaceTest, DeletingAnAncestorThatWasEvictedMidFlightStillRejectsTheChild) {
  // The reason a lease snapshots its conversation instead of re-walking it at delete time: the parent can be
  // compacted away while the request runs, and its id would then match no stored entry.
  ResponseStore store(2);
  store.Store("resp_1", ResponseJson("resp_1"), json::array(), "model-a");
  store.Store("resp_2", ResponseJson("resp_2", "resp_1"), json::array(), "model-a");

  auto continuation = store.BeginResponse("resp_2", "model-a");
  ASSERT_EQ(continuation.status, ContinuationStatus::kOk);

  // A third response pushes the conversation's root out of the store; it survives only as a compacted prefix.
  store.Store("resp_x", ResponseJson("resp_x"), json::array(), "model-a");
  ASSERT_FALSE(store.Get("resp_1").has_value());

  ASSERT_TRUE(store.Delete("resp_1")) << "a compacted ancestor must still be deletable";

  RecordingAdmission admission;
  EXPECT_FALSE(store.Commit(
      continuation.lease,
      ResponseStore::StoredResponse{"resp_3", "model-a", ResponseJson("resp_3", "resp_2"), json::array()},
      &admission));
  EXPECT_TRUE(admission.admitted.empty());
}

TEST(ResponseDeleteRaceTest, DeletingAParentAfterCapacityEvictionStillInvalidatesItsLease) {
  ResponseStore store(1);
  store.Store("parent", ResponseJson("parent"), json::array(), "model-a");

  auto continuation = store.BeginResponse("parent", "model-a");
  ASSERT_EQ(continuation.status, ContinuationStatus::kOk);

  store.Store("unrelated", ResponseJson("unrelated"), json::array(), "model-a");
  ASSERT_FALSE(store.Get("parent").has_value());
  EXPECT_FALSE(store.Delete("parent")) << "capacity-evicted metadata remains client-invisible";

  RecordingAdmission admission;
  EXPECT_FALSE(store.Commit(
      continuation.lease,
      ResponseStore::StoredResponse{"child", "model-a", ResponseJson("child", "parent"), json::array()},
      &admission));
  EXPECT_TRUE(admission.admitted.empty());
  EXPECT_FALSE(store.Get("child").has_value());
}

TEST(ResponseDeleteRaceTest, OnlyTheDependentOfTwoConcurrentRequestsIsRejected) {
  ResponseStore store;
  store.Store("resp_1", ResponseJson("resp_1"), json::array(), "model-a");
  store.Store("other_1", ResponseJson("other_1"), json::array(), "model-a");

  auto dependent = store.BeginResponse("resp_1", "model-a");
  auto independent = store.BeginResponse("other_1", "model-a");
  ASSERT_EQ(store.InFlightResponses(), 2u);

  ASSERT_TRUE(store.Delete("resp_1"));

  EXPECT_FALSE(store.Commit(
      dependent.lease,
      ResponseStore::StoredResponse{"resp_2", "model-a", ResponseJson("resp_2", "resp_1"), json::array()}, nullptr));
  EXPECT_TRUE(store.Commit(
      independent.lease,
      ResponseStore::StoredResponse{"other_2", "model-a", ResponseJson("other_2", "other_1"), json::array()},
      nullptr));
  EXPECT_EQ(store.InFlightResponses(), 0u);
}

TEST(ResponseDeleteRaceTest, ADeleteThatArrivesAfterTheCommitDropsTheCachedSession) {
  // The other side of the race: once the commit has published both the metadata and the session, the delete finds
  // the child as a dependent and removes both.
  RecordingCache cache;
  ResponseStore store(ResponseStore::kDefaultCapacity, &cache);
  RecordingAdmission admission;

  ASSERT_TRUE(RunTurn(store, "resp_1", "", "model-a", &admission));
  ASSERT_TRUE(RunTurn(store, "resp_2", "resp_1", "model-a", &admission));

  const auto deleted = store.DeleteWithDependents("resp_1");

  EXPECT_EQ(deleted.size(), 2u);
  EXPECT_EQ(cache.dropped, deleted) << "every deleted response must have its cached session dropped";
  EXPECT_FALSE(store.Get("resp_2").has_value());
}

TEST(ResponseDeleteRaceTest, DeletingAnUnknownResponseDropsNothing) {
  RecordingCache cache;
  ResponseStore store(ResponseStore::kDefaultCapacity, &cache);
  store.Store("resp_1", ResponseJson("resp_1"), json::array(), "model-a");

  EXPECT_FALSE(store.Delete("resp_missing"));
  EXPECT_TRUE(cache.dropped.empty());
}

TEST(ResponseDeleteRaceTest, DeletingACompactedAncestorDropsTheSessionCachedUnderItsOwnId) {
  RecordingCache cache;
  ResponseStore store(2, &cache);
  store.Store("resp_1", ResponseJson("resp_1"), json::array(), "model-a");
  store.Store("resp_2", ResponseJson("resp_2", "resp_1"), json::array(), "model-a");
  store.Store("resp_x", ResponseJson("resp_x"), json::array(), "model-a");
  ASSERT_FALSE(store.Get("resp_1").has_value());
  ASSERT_EQ(cache.dropped, (std::vector<std::string>{"resp_1"}));
  cache.dropped.clear();

  const auto deleted = store.DeleteWithDependents("resp_1");

  // resp_2 retains resp_1's content in its compacted prefix, so both ids must be purged from the cache.
  EXPECT_EQ(deleted, (std::vector<std::string>{"resp_2", "resp_1"}));
  EXPECT_EQ(cache.dropped, deleted);
}

namespace {

/// Both halves of the cache contract, recorded under one mutex so a concurrent delete and commit can be replayed
/// afterwards.
class ConcurrentCacheRecorder final : public IResponseAdmission, public IResponseCacheCoordinator {
 public:
  void Admit(const std::string& response_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    admitted_.push_back(response_id);
  }

  void Drop(const std::string& response_id) noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    dropped_.push_back(response_id);
  }

  bool WasAdmitted(const std::string& response_id) const { return Contains(admitted_, response_id); }
  bool WasDropped(const std::string& response_id) const { return Contains(dropped_, response_id); }

 private:
  bool Contains(const std::vector<std::string>& ids, const std::string& response_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::find(ids.begin(), ids.end(), response_id) != ids.end();
  }

  mutable std::mutex mutex_;
  std::vector<std::string> admitted_;
  std::vector<std::string> dropped_;
};

}  // namespace

TEST(ResponseDeleteRaceTest, ADeleteRacingACommitNeverLeavesTheChildCachedAfterItsAncestorIsGone) {
  // The invariant the lease exists to guarantee, exercised against a real interleaving: whichever order the two
  // operations land in, a response that survives in the cache must also survive in the store. Admitting outside the
  // store's critical section would let the delete slip between the two and reopen the window.
  for (int attempt = 0; attempt < 200; ++attempt) {
    ConcurrentCacheRecorder recorder;
    ResponseStore store(ResponseStore::kDefaultCapacity, &recorder);
    store.Store("root", ResponseJson("root"), json::array(), "model-a");

    auto continuation = store.BeginResponse("root", "model-a");
    ASSERT_EQ(continuation.status, ContinuationStatus::kOk);

    std::thread deleter([&store] { store.Delete("root"); });

    const bool committed = store.Commit(
        continuation.lease,
        ResponseStore::StoredResponse{"child", "model-a", ResponseJson("child", "root"), json::array()}, &recorder);

    deleter.join();

    if (committed) {
      // The delete ran after the commit, so it had to find and drop the child it created.
      EXPECT_TRUE(recorder.WasAdmitted("child"));
      EXPECT_TRUE(recorder.WasDropped("child")) << "child stayed cached after its ancestor was deleted";
    } else {
      EXPECT_FALSE(recorder.WasAdmitted("child")) << "child was cached even though its ancestor was deleted";
    }

    EXPECT_FALSE(store.Get("child").has_value());
    EXPECT_EQ(store.InFlightResponses(), 0u);
  }
}
