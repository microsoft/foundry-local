// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/generative/openresponses/response_chain.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fl {

class ResponseStore;

/// Admission of the live artifact a completed response leaves behind — the warm ChatSession a following turn would
/// reuse. The store invokes this while holding its own lock, so a response's metadata and its cached session become
/// visible as one step: a concurrent delete either sees neither or both.
///
/// The store never owns or names the artifact; the service layer implements this over its session cache.
class IResponseAdmission {
 public:
  virtual ~IResponseAdmission() = default;

  /// Publish the artifact under `response_id`. Called at most once, and never after the lease was invalidated.
  /// If this throws, it must leave no artifact published.
  virtual void Admit(const std::string& response_id) = 0;
};

/// Eviction of live artifacts whose response metadata the store has removed. Called for every deleted response id,
/// inside the store's lock, so a cached session cannot outlive its metadata for any observable window.
class IResponseCacheCoordinator {
 public:
  virtual ~IResponseCacheCoordinator() = default;

  /// Drop anything cached under `response_id`. Must not throw or call back into the store — see ResponseStore's lock
  /// order. No-throw publication lets capacity eviction stay atomic with metadata replacement.
  virtual void Drop(const std::string& response_id) noexcept = 0;
};

/// Why a response could not be started as a continuation of another.
enum class ContinuationStatus {
  kOk,
  /// The previous response is unknown, was evicted without a replay prefix, or the stored links form a cycle.
  kChainUnavailable,
  /// The previous response was produced by a different model, so neither its warm session nor its replayed
  /// transcript belongs to this request's model.
  kModelMismatch,
};

/// RAII lease over one in-flight response. Held for the whole request: from the moment its conversation is validated
/// until its result is committed (or abandoned).
///
/// The lease is what makes deletion and generation coherent. Deleting any ancestor of the conversation invalidates
/// every lease that depends on it, so a request that started before the delete cannot publish a child that carries
/// the deleted content forward — neither as stored metadata nor as a warm cached session.
class ResponseLease {
 public:
  ResponseLease() = default;
  ~ResponseLease();

  ResponseLease(ResponseLease&& other) noexcept;
  ResponseLease& operator=(ResponseLease&& other) noexcept;

  ResponseLease(const ResponseLease&) = delete;
  ResponseLease& operator=(const ResponseLease&) = delete;

  /// True while this lease still refers to a registered in-flight response.
  explicit operator bool() const noexcept { return store_ != nullptr; }

  /// Abandon the in-flight response without committing it. Idempotent; also run by the destructor.
  void Release() noexcept;

 private:
  friend class ResponseStore;

  ResponseLease(ResponseStore& store, uint64_t id) noexcept : store_(&store), id_(id) {}

  ResponseStore* store_ = nullptr;
  uint64_t id_ = 0;
};

/// In-memory LRU store for Responses API responses and their input items.
/// Thread-safe. Usable by both the web service handler and the direct
/// ResponsesClient API.
///
/// Stores responses as JSON objects to avoid tight coupling with the
/// Responses API type hierarchy — the handler builds JSON, the store
/// keeps it.
class ResponseStore {
 public:
  static constexpr int kDefaultCapacity = 20;
  static constexpr int kMaxCapacity = 100;

  /// Deterministic fault points for validating the metadata transaction's strong exception guarantee.
  enum class StorePhase {
    kAfterEntriesCloned,
    kAfterIndexRebuilt,
    kAfterEntryUpdated,
    kAfterCompaction,
  };
  using StoreFaultInjector = std::function<void(StorePhase)>;

  /// @param capacity  Maximum number of retained response metadata entries.
  /// @param cache     Optional live-artifact cache kept in step with deletions. nullptr = the store owns no cache.
  /// @param fault_injector  Optional deterministic test seam invoked while preparing metadata.
  explicit ResponseStore(int capacity = kDefaultCapacity,
                         IResponseCacheCoordinator* cache = nullptr,
                         StoreFaultInjector fault_injector = {});

  /// Everything a completed response contributes to the store.
  struct StoredResponse {
    std::string id;
    /// Resolved model that produced it. A continuation must resolve to the same model; empty means unbound, which
    /// constrains nothing (only callers that do not participate in model routing store unbound responses).
    std::string model_id;
    /// The response object as returned on the wire.
    nlohmann::json response;
    /// This hop's own request items — exactly what the /input_items endpoint returns.
    nlohmann::json input_items;
  };

  /// Outcome of ResponseStore::BeginResponse.
  struct ContinuationResult {
    ContinuationStatus status = ContinuationStatus::kOk;
    /// Held only when `status == kOk`.
    ResponseLease lease;
    /// The model that produced the previous response. Set when `status == kModelMismatch` so the caller can say
    /// which model the conversation actually belongs to.
    std::string parent_model_id;
  };

  /// Open a lease for a request before any inference runs.
  ///
  /// For a continuation (`previous_response_id` non-empty) this is the single admission gate: it checks that the
  /// chain is intact, that the previous response was produced by `model_id`, and keeps every hop of the chain warm.
  /// Validating the model here — before the caller decides between a cached session and a replayed transcript —
  /// is what keeps warm and cold continuation consistent: a chain belonging to another model is refused on both
  /// paths instead of reusing that model's session and labelling the answer with this one.
  ///
  /// An empty `previous_response_id` starts a new conversation: always `kOk`, with a lease that no deletion can
  /// invalidate.
  [[nodiscard]] ContinuationResult BeginResponse(const std::string& previous_response_id,
                                                 const std::string& model_id);

  /// Publish the result of a leased request: admit its live artifact and store its metadata in one critical section.
  ///
  /// Returns false when an ancestor of the conversation was deleted while the request was running. Nothing is then
  /// stored and `admission` is never invoked, so no descendant of deleted content survives in either the store or
  /// the caller's cache. Consumes the lease either way — unless it belongs to a different store, which is refused
  /// without touching it.
  ///
  /// @param admission  Optional; nullptr when the caller keeps no live artifact for this response.
  bool Commit(ResponseLease& lease, StoredResponse response, IResponseAdmission* admission);

  /// Number of leases currently outstanding. Diagnostics and tests — the value is a snapshot.
  size_t InFlightResponses() const;

  /// Store a completed response and its input items atomically.
  /// Complements SessionManager's cached ChatSession instances: SessionManager caches a small number of live sessions
  /// (with their generators / KV cache) for fast continuation, while ResponseStore keeps a larger, lightweight history
  /// of completed responses and their input items for lookup and pagination beyond the session cache capacity.
  ///
  /// Eviction removes response metadata from lookup and pagination, but compacts the evicted hop into any retained
  /// child. A complete stored chain therefore stays reconstructable after a session-cache miss as it exceeds capacity.
  /// The entry capacity bounds response metadata, not replay history: compacted input/output remains resident while a
  /// retained descendant needs it. Growth stops when the replayed conversation no longer fits a successful model turn.
  ///
  /// Direct entry point for callers outside the request lifecycle (fixtures and tests). Every request that runs
  /// inference goes through BeginResponse/Commit instead, which binds the model and coordinates with deletion.
  /// An entry stored here without a `model_id` is unbound and constrains no later continuation.
  void Store(const std::string& response_id,
             nlohmann::json response,
             nlohmann::json input_items,
             std::string model_id = {});

  /// Retrieve a stored response by ID. Returns nullopt if not found.
  std::optional<nlohmann::json> Get(const std::string& response_id);

  /// Retrieve stored input items for a response. Returns nullopt if not found.
  std::optional<nlohmann::json> GetInputItems(const std::string& response_id);

  /// Reconstruct the complete replay context for a chained response: every hop from the root of the
  /// `previous_response_id` chain to `response_id`, oldest first, each keeping its own input items and its own
  /// output items.
  ///
  /// Each entry stores only its own request's input items — that is exactly what the /input_items endpoint must
  /// return — so a caller that replayed a single hop would lose everything before it, including the assistant tool
  /// calls that later tool results have to correlate against.
  ///
  /// The per-hop grouping is part of the contract: a hop's output is one assistant turn, and replay has to rebuild
  /// it as one assistant message even when that turn produced only reasoning or nothing at all.
  ///
  /// A hop's input items are returned exactly as stored. Instructions are request-scoped and are not stored as
  /// items at all, so the store never has to guess which system message was the caller's — every system message a
  /// caller sent is replayed verbatim.
  ///
  /// Returns nullopt when the chain cannot be reconstructed: a link was never stored (or was explicitly deleted)
  /// and has no compacted replay prefix, or the stored links form a cycle. Callers must fail explicitly rather than
  /// run inference on a truncated conversation.
  std::optional<ResponseChainContext> BuildChainContext(const std::string& response_id);

  /// Delete a response and every retained descendant whose replay depends on it. Returns true if the response existed
  /// either as stored metadata or inside a compacted replay prefix.
  bool Delete(const std::string& response_id);

  /// Delete a response and every retained response whose replay depends on it. This also finds an evicted response
  /// inside a compacted prefix, so content that remains replayable never becomes undeletable.
  ///
  /// Deletion also reaches forward in time: every in-flight lease whose conversation contains one of the deleted
  /// responses is invalidated, and the cache coordinator drops the live session of every deleted id. Both happen in
  /// this call's critical section, so a request that is mid-generation cannot afterwards publish a child of deleted
  /// content, and a session cannot be checked in under an id that was just removed.
  ///
  /// Returns the IDs whose response metadata was removed; an empty result means the response was unknown.
  std::vector<std::string> DeleteWithDependents(const std::string& response_id);

  /// One page of stored responses plus the exact continuation state.
  struct Page {
    std::vector<nlohmann::json> data;
    /// True when at least one more response exists after this page. Derived from the store's own iteration, so a
    /// final page that happens to hold exactly `limit` items is reported correctly.
    bool has_more = false;
  };

  /// List stored responses with cursor-based pagination.
  /// @param limit  Maximum number to return.
  /// @param after  Cursor — return responses after this ID. Empty (or unknown) = from the start.
  /// @param order  "asc" or "desc" (default: "desc" = newest first).
  /// @return  One page of response JSON objects and whether more follow it.
  Page List(int limit = 20, const std::string& after = "", const std::string& order = "desc");

  /// Number of responses currently stored.
  size_t Size() const;

 private:
  friend class ResponseLease;

  /// Persistent replay prefix shared by every retained branch below an evicted hop. The head is the newest compacted
  /// hop and `previous` walks toward the conversation root, so compacting a branch copies no prior input/output data.
  struct ReplayPrefix {
    std::string id;
    std::shared_ptr<const ReplayPrefix> previous;
    ResponseChainHop hop;
  };

  struct Entry {
    std::string id;
    std::string model_id;
    nlohmann::json response;
    nlohmann::json input_items;
    std::shared_ptr<const ReplayPrefix> replay_prefix;
    uint64_t insertion_sequence = 0;
  };

  using EntryList = std::list<Entry>;
  using EntryIndex = std::unordered_map<std::string, EntryList::iterator>;

  struct MetadataState {
    EntryList entries;
    EntryIndex index;
    std::vector<std::string> evicted_ids;
    uint64_t next_insertion_sequence = 0;
  };

  /// One in-flight response registered by BeginResponse.
  ///
  /// `ancestors` is a snapshot of the conversation taken while the chain was validated, rather than a live lookup at
  /// delete time: the parent itself may be evicted and compacted away while this request generates, and its id would
  /// then no longer resolve to anything a delete could match. Its size is the depth of the chain — the same order as
  /// the ReplayPrefix chain the store already retains — times the number of concurrently generating requests, so the
  /// state stays bounded by in-flight work and is released with the lease.
  struct PendingResponse {
    std::unordered_set<std::string> ancestors;
    /// Immutable complete replay state through the leased endpoint. Capacity eviction may remove every corresponding
    /// metadata entry while inference runs; the snapshot lets Commit repair only the missing prefix without pinning
    /// those entries or exposing additional response data to clients.
    std::shared_ptr<const ReplayPrefix> replay_snapshot;
    bool invalidated = false;
  };

  struct WalkedChain {
    std::vector<std::list<Entry>::iterator> hops;
    /// The compacted prefix that joins the oldest resident hop to the missing portion of its ancestry.
    std::shared_ptr<const ReplayPrefix> replay_prefix;
  };

  int capacity_;
  IResponseCacheCoordinator* cache_;
  StoreFaultInjector fault_injector_;

  /// Guards every member below. Lock order: this mutex may be held while calling into IResponseCacheCoordinator or
  /// IResponseAdmission (which reach into the service's session cache), so neither may call back into the store.
  mutable std::mutex mutex_;
  EntryList entries_;  // front = most recently used; List orders by Entry::insertion_sequence instead
  EntryIndex index_;
  std::unordered_map<uint64_t, PendingResponse> pending_;
  uint64_t next_lease_id_ = 1;
  uint64_t next_insertion_sequence_ = 0;

  MetadataState PrepareStoreLocked(StoredResponse response,
                                   const std::shared_ptr<const ReplayPrefix>& replay_snapshot = nullptr);
  void CommitStoreLocked(MetadataState&& state) noexcept;
  void DropEvictedArtifactsLocked(const MetadataState& state);
  void StoreLocked(StoredResponse response, MetadataState& state);
  void AttachReplaySnapshotIfNeededLocked(MetadataState& state,
                                          const std::string& response_id,
                                          const std::shared_ptr<const ReplayPrefix>& replay_snapshot);
  void Evict(MetadataState& state);
  void CompactAndEraseLocked(MetadataState& state, EntryList::iterator root);
  void InjectStoreFault(StorePhase phase) const;
  void ReleaseLease(uint64_t lease_id) noexcept;
  static ResponseChainHop ToReplayHop(const Entry& entry);
  static bool PrefixContains(const std::shared_ptr<const ReplayPrefix>& prefix, const std::string& response_id);
  bool DependsOnLocked(const Entry& entry, const std::string& response_id) const;
  void TouchLocked(std::list<Entry>::iterator it);

  /// Walk `response_id` back to the oldest retained hop. Returns stored hops newest-first and the prefix that fills
  /// any missing ancestry, or no hops when a link is missing without a usable prefix or the links form a cycle.
  WalkedChain WalkChainLocked(const std::string& response_id);

  /// Every response id a walked chain depends on: its own hops plus the ids folded into its compacted prefix.
  static std::unordered_set<std::string> ChainIdsLocked(const WalkedChain& chain);
  static std::shared_ptr<const ReplayPrefix> SnapshotChainLocked(const WalkedChain& chain);
};

}  // namespace fl
