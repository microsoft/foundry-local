// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inferencing/generative/openresponses/response_store.h"

#include <algorithm>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fl {

// --- ResponseLease ---

ResponseLease::~ResponseLease() {
  Release();
}

ResponseLease::ResponseLease(ResponseLease&& other) noexcept
    : store_(std::exchange(other.store_, nullptr)), id_(std::exchange(other.id_, 0)) {
}

ResponseLease& ResponseLease::operator=(ResponseLease&& other) noexcept {
  if (this != &other) {
    Release();
    store_ = std::exchange(other.store_, nullptr);
    id_ = std::exchange(other.id_, 0);
  }

  return *this;
}

void ResponseLease::Release() noexcept {
  if (store_ != nullptr) {
    store_->ReleaseLease(id_);
    store_ = nullptr;
    id_ = 0;
  }
}

// --- ResponseStore ---

ResponseStore::ResponseStore(int capacity, IResponseCacheCoordinator* cache, StoreFaultInjector fault_injector)
    : capacity_(std::clamp(capacity, 1, kMaxCapacity)),
      cache_(cache),
      fault_injector_(std::move(fault_injector)) {
}

void ResponseStore::Store(const std::string& response_id,
                          nlohmann::json response,
                          nlohmann::json input_items,
                          std::string model_id,
                          StoredToolKinds output_tool_kinds) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto state = PrepareStoreLocked({response_id, std::move(model_id), std::move(response),
                                   std::move(input_items), std::move(output_tool_kinds)});
  DropEvictedArtifactsLocked(state);
  CommitStoreLocked(std::move(state));
}

ResponseStore::MetadataState ResponseStore::PrepareStoreLocked(
    StoredResponse response, const std::shared_ptr<const ReplayPrefix>& replay_snapshot) {
  const auto response_id = response.id;
  MetadataState state;
  state.entries = entries_;
  state.next_insertion_sequence = next_insertion_sequence_;
  InjectStoreFault(StorePhase::kAfterEntriesCloned);

  state.index.reserve(index_.size() + 1);
  for (auto it = state.entries.begin(); it != state.entries.end(); ++it) {
    state.index.emplace(it->id, it);
  }
  InjectStoreFault(StorePhase::kAfterIndexRebuilt);

  StoreLocked(std::move(response), state);
  AttachReplaySnapshotIfNeededLocked(state, response_id, replay_snapshot);
  return state;
}

void ResponseStore::CommitStoreLocked(MetadataState&& state) noexcept {
  entries_.swap(state.entries);
  index_.swap(state.index);
  next_insertion_sequence_ = state.next_insertion_sequence;
}

void ResponseStore::DropEvictedArtifactsLocked(const MetadataState& state) {
  if (cache_ == nullptr) {
    return;
  }

  for (const auto& evicted_id : state.evicted_ids) {
    cache_->Drop(evicted_id);
  }
}

void ResponseStore::StoreLocked(StoredResponse response, MetadataState& state) {
  std::shared_ptr<const ReplayPrefix> replay_prefix;

  // If already exists, preserve its compacted ancestry when the replacement keeps the same parent.
  auto it = state.index.find(response.id);
  if (it != state.index.end()) {
    const auto old_previous = it->second->response.find("previous_response_id");
    const auto new_previous = response.response.find("previous_response_id");
    if (old_previous != it->second->response.end() && new_previous != response.response.end() &&
        *old_previous == *new_previous) {
      replay_prefix = it->second->replay_prefix;
    }

    state.entries.erase(it->second);
    state.index.erase(it);
  }

  // Insert at front (most recently used)
  state.entries.push_front(Entry{.id = response.id,
                                 .model_id = std::move(response.model_id),
                                 .response = std::move(response.response),
                                 .input_items = std::move(response.input_items),
                                 .output_tool_kinds = std::move(response.output_tool_kinds),
                                 .replay_prefix = std::move(replay_prefix),
                                 .insertion_sequence = state.next_insertion_sequence++});
  state.index[response.id] = state.entries.begin();
  InjectStoreFault(StorePhase::kAfterEntryUpdated);

  Evict(state);
}

void ResponseStore::AttachReplaySnapshotIfNeededLocked(
    MetadataState& state,
    const std::string& response_id,
    const std::shared_ptr<const ReplayPrefix>& replay_snapshot) {
  if (replay_snapshot == nullptr) {
    return;
  }

  const auto child = state.index.find(response_id);
  if (child == state.index.end()) {
    return;
  }

  auto current = child->second;
  std::unordered_set<std::string> visited;
  while (visited.insert(current->id).second) {
    const auto previous = current->response.find("previous_response_id");
    if (previous == current->response.end() || !previous->is_string() || previous->get<std::string>().empty()) {
      return;
    }

    const auto previous_id = previous->get<std::string>();
    const auto parent = state.index.find(previous_id);
    if (parent != state.index.end()) {
      current = parent->second;
      continue;
    }

    // Normal compaction already repaired this exact boundary. Keep it rather than layering the lease snapshot over
    // resident history, which would retain duplicate hops.
    if (current->replay_prefix != nullptr && current->replay_prefix->id == previous_id) {
      return;
    }

    for (auto snapshot = replay_snapshot; snapshot != nullptr; snapshot = snapshot->previous) {
      if (snapshot->id == previous_id) {
        child->second->replay_prefix = std::move(snapshot);
        return;
      }
    }

    return;
  }
}

ResponseStore::ContinuationResult ResponseStore::BeginResponse(const std::string& previous_response_id,
                                                               const std::string& model_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  PendingResponse pending;

  if (!previous_response_id.empty()) {
    auto chain = WalkChainLocked(previous_response_id);
    if (chain.hops.empty()) {
      ContinuationResult unavailable;
      unavailable.status = ContinuationStatus::kChainUnavailable;
      return unavailable;
    }

    // The chain belongs to the model that produced it: its cached session holds that model's KV cache, and its
    // replayed transcript was written by that model's template. Refuse before anything is reused, so warm and cold
    // continuation agree and no answer is labelled with a model that did not produce it.
    const std::string& parent_model_id = chain.hops.front()->model_id;
    if (!parent_model_id.empty() && !model_id.empty() && parent_model_id != model_id) {
      ContinuationResult mismatch;
      mismatch.status = ContinuationStatus::kModelMismatch;
      mismatch.parent_model_id = parent_model_id;
      return mismatch;
    }

    // Keep the whole chain warm even when the caller continues from a live session and never rebuilds it: without
    // this the conversation's own early hops would age out while it is still active, and a later session-cache miss
    // could no longer reconstruct it. Touch oldest-first so the requested endpoint ends up most recent.
    for (auto hop = chain.hops.rbegin(); hop != chain.hops.rend(); ++hop) {
      TouchLocked(*hop);
    }

    pending.ancestors = ChainIdsLocked(chain);
    pending.replay_snapshot = SnapshotChainLocked(chain);
  }

  const uint64_t lease_id = next_lease_id_++;
  pending_.emplace(lease_id, std::move(pending));

  ContinuationResult accepted;
  accepted.lease = ResponseLease(*this, lease_id);
  return accepted;
}

bool ResponseStore::Commit(ResponseLease& lease, StoredResponse response, IResponseAdmission* admission) {
  std::lock_guard<std::mutex> lock(mutex_);

  // A lease issued by a different store is not ours to consume: clearing it here would orphan that store's
  // registration forever. Refuse and leave it to its owner.
  if (lease.store_ != this) {
    return false;
  }

  // Our own lease is consumed either way: a rejected commit is final, and the caller must not retry into a
  // conversation whose ancestor is gone.
  auto pending = pending_.find(lease.id_);
  const bool valid = pending != pending_.end() && !pending->second.invalidated;
  std::shared_ptr<const ReplayPrefix> replay_snapshot;

  if (pending != pending_.end()) {
    replay_snapshot = std::move(pending->second.replay_snapshot);
    pending_.erase(pending);
  }

  lease.store_ = nullptr;
  lease.id_ = 0;

  if (!valid) {
    return false;
  }

  const auto response_id = response.id;

  // Build the complete post-commit metadata state before publishing either half. Copying JSON, rebuilding the index,
  // replacement, eviction, and replay-prefix compaction can all allocate; failures leave the live state untouched.
  auto state = PrepareStoreLocked(std::move(response), replay_snapshot);

  // Admission is the final throwing operation. Capacity drops and metadata publication are no-throw, so a failed
  // admission preserves both the old metadata and its cached sessions exactly.
  if (admission != nullptr) {
    admission->Admit(response_id);
  }

  // The store lock preserves the documented store -> session-manager lock order throughout.
  DropEvictedArtifactsLocked(state);
  CommitStoreLocked(std::move(state));

  return true;
}

size_t ResponseStore::InFlightResponses() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_.size();
}

void ResponseStore::ReleaseLease(uint64_t lease_id) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  pending_.erase(lease_id);
}

std::optional<nlohmann::json> ResponseStore::Get(const std::string& response_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = index_.find(response_id);
  if (it == index_.end()) {
    return std::nullopt;
  }

  TouchLocked(it->second);
  return it->second->response;
}

std::optional<nlohmann::json> ResponseStore::GetInputItems(const std::string& response_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = index_.find(response_id);
  if (it == index_.end()) {
    return std::nullopt;
  }

  TouchLocked(it->second);
  return it->second->input_items;
}

ResponseStore::WalkedChain ResponseStore::WalkChainLocked(const std::string& response_id) {
  // A visited set bounds the walk: cyclic links would otherwise loop forever, and a cycle means the stored chain is
  // not a conversation that can be replayed.
  WalkedChain chain;
  std::unordered_set<std::string> visited;
  std::string id = response_id;

  while (!id.empty()) {
    if (!visited.insert(id).second) {
      return {};
    }

    auto it = index_.find(id);
    if (it == index_.end()) {
      return {};
    }

    chain.hops.push_back(it->second);

    const auto previous = it->second->response.find("previous_response_id");
    if (previous == it->second->response.end() || !previous->is_string()) {
      chain.replay_prefix = it->second->replay_prefix;
      break;
    }

    id = previous->get<std::string>();
    if (id.empty()) {
      chain.replay_prefix = it->second->replay_prefix;
      break;
    }

    if (index_.find(id) == index_.end()) {
      const auto& oldest_prefix = chain.hops.back()->replay_prefix;
      if (oldest_prefix != nullptr && oldest_prefix->id == id) {
        chain.replay_prefix = oldest_prefix;
        return chain;
      }

      const auto& endpoint_prefix = chain.hops.front()->replay_prefix;
      if (endpoint_prefix != nullptr && endpoint_prefix->id == id) {
        chain.replay_prefix = endpoint_prefix;
        return chain;
      }

      return {};
    }
  }

  return chain;
}

std::optional<ResponseChainContext> ResponseStore::BuildChainContext(const std::string& response_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto chain = WalkChainLocked(response_id);
  if (chain.hops.empty()) {
    return std::nullopt;
  }

  // The walk collected hops newest-first; replay oldest-first so each hop's tool calls precede the results for them.
  ResponseChainContext context;
  std::vector<const ReplayPrefix*> prefix;
  for (auto node = chain.replay_prefix.get(); node != nullptr; node = node->previous.get()) {
    prefix.push_back(node);
  }

  context.reserve(prefix.size() + chain.hops.size());
  for (auto hop = prefix.rbegin(); hop != prefix.rend(); ++hop) {
    context.push_back((*hop)->hop);
  }

  for (auto hop = chain.hops.rbegin(); hop != chain.hops.rend(); ++hop) {
    context.push_back(ToReplayHop(**hop));
  }

  // Touch oldest-first so the requested endpoint finishes at the front as the most-recent entry. Losing any ancestor
  // breaks the chain, but evicting the endpoint first would strand every ancestor without preserving continuation.
  // list::splice keeps the collected iterators valid.
  for (auto hop = chain.hops.rbegin(); hop != chain.hops.rend(); ++hop) {
    TouchLocked(*hop);
  }

  return context;
}

bool ResponseStore::Delete(const std::string& response_id) {
  return !DeleteWithDependents(response_id).empty();
}

std::vector<std::string> ResponseStore::DeleteWithDependents(const std::string& response_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  std::vector<std::list<Entry>::iterator> dependents;
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (DependsOnLocked(*it, response_id)) {
      dependents.push_back(it);
    }
  }

  std::vector<std::string> deleted_ids;
  deleted_ids.reserve(dependents.size() + 1);
  for (auto dependent : dependents) {
    deleted_ids.push_back(dependent->id);
    index_.erase(dependent->id);
    entries_.erase(dependent);
  }

  // A compacted response has no metadata entry of its own, but its ID must still be returned so the handler can evict
  // a session cached under the exact ID the caller deleted.
  if (!deleted_ids.empty() &&
      std::find(deleted_ids.begin(), deleted_ids.end(), response_id) == deleted_ids.end()) {
    deleted_ids.push_back(response_id);
  }

  // Requests already generating cannot be recalled, but their results can be refused: any conversation that contains
  // a deleted response may no longer produce a stored or cached child. Match the requested id directly as well:
  // capacity eviction may have removed its last metadata representation while a lease still owns its replay snapshot.
  const std::unordered_set<std::string> deleted(deleted_ids.begin(), deleted_ids.end());
  for (auto& [lease_id, pending] : pending_) {
    if (pending.invalidated) {
      continue;
    }

    pending.invalidated =
        pending.ancestors.count(response_id) != 0 ||
        std::any_of(pending.ancestors.begin(), pending.ancestors.end(),
                    [&deleted](const std::string& id) { return deleted.count(id) != 0; });
  }

  if (deleted_ids.empty()) {
    return deleted_ids;
  }

  // Drop live sessions inside this critical section. Doing it after the lock would leave a window in which a session
  // is still checked out under an id whose metadata is already gone.
  if (cache_ != nullptr) {
    for (const auto& deleted_id : deleted_ids) {
      cache_->Drop(deleted_id);
    }
  }

  return deleted_ids;
}

ResponseStore::Page ResponseStore::List(int limit, const std::string& after, const std::string& order) {
  std::lock_guard<std::mutex> lock(mutex_);

  // LRU touches must never move a pagination cursor. Order by the immutable insertion sequence rather than by the
  // recency list used for capacity eviction.
  std::vector<const Entry*> ordered;
  ordered.reserve(entries_.size());
  for (const auto& entry : entries_) {
    ordered.push_back(&entry);
  }
  std::sort(ordered.begin(), ordered.end(), [](const Entry* left, const Entry* right) {
    return left->insertion_sequence > right->insertion_sequence;
  });

  // For ascending order, reverse so oldest is first
  if (order == "asc") {
    std::reverse(ordered.begin(), ordered.end());
  }

  // Apply cursor — skip entries until we find the "after" ID
  auto start = ordered.begin();
  if (!after.empty()) {
    for (auto it = ordered.begin(); it != ordered.end(); ++it) {
      if ((*it)->id == after) {
        start = it + 1;
        break;
      }
    }
  }

  // Collect results up to limit
  Page page;
  auto it = start;
  for (int count = 0; it != ordered.end() && count < limit; ++it, ++count) {
    page.data.push_back((*it)->response);
  }

  // The iterator says exactly whether anything follows the page. Reporting `data.size() == limit` instead claimed a
  // next page whenever the last page happened to be exactly full, and the caller's follow-up request came back empty.
  page.has_more = it != ordered.end();

  return page;
}

size_t ResponseStore::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

void ResponseStore::Evict(MetadataState& state) {
  while (static_cast<int>(state.entries.size()) > capacity_) {
    // Evict within the least-recently-used conversation. Walk that entry toward its root so an internal hop is never
    // removed between a retained parent and child. A cycle is malformed; evict the LRU entry itself so malformed
    // conversations cannot become immortal and force every newer response back out of the store.
    auto lru = std::prev(state.entries.end());
    auto root = lru;
    std::unordered_set<std::string> visited;
    bool cycle = false;

    while (true) {
      if (!visited.insert(root->id).second) {
        cycle = true;
        break;
      }

      const auto previous = root->response.find("previous_response_id");
      if (previous == root->response.end() || !previous->is_string() || previous->get<std::string>().empty()) {
        break;
      }

      const auto parent = state.index.find(previous->get<std::string>());
      if (parent == state.index.end()) {
        break;
      }

      root = parent->second;
    }

    if (cycle) {
      root = lru;
    }

    CompactAndEraseLocked(state, root);
  }
}

void ResponseStore::CompactAndEraseLocked(MetadataState& state, EntryList::iterator root) {
  const auto previous = root->response.find("previous_response_id");
  const bool is_complete_root = previous == root->response.end() || !previous->is_string() ||
                                previous->get<std::string>().empty() || root->replay_prefix != nullptr;

  if (is_complete_root) {
    const auto prefix = std::make_shared<ReplayPrefix>(
        ReplayPrefix{root->id, root->replay_prefix, ToReplayHop(*root)});

    // Branches share the immutable prefix rather than copying every earlier tool result into every child.
    for (auto& entry : state.entries) {
      const auto entry_previous = entry.response.find("previous_response_id");
      if (entry_previous != entry.response.end() && entry_previous->is_string() &&
          entry_previous->get<std::string>() == root->id) {
        entry.replay_prefix = prefix;
      }
    }
  }

  state.evicted_ids.push_back(root->id);
  state.index.erase(root->id);
  state.entries.erase(root);
  InjectStoreFault(StorePhase::kAfterCompaction);
}

void ResponseStore::InjectStoreFault(StorePhase phase) const {
  if (fault_injector_) {
    fault_injector_(phase);
  }
}

bool ResponseStore::PrefixContains(const std::shared_ptr<const ReplayPrefix>& prefix,
                                   const std::string& response_id) {
  for (auto node = prefix.get(); node != nullptr; node = node->previous.get()) {
    if (node->id == response_id) {
      return true;
    }
  }

  return false;
}

bool ResponseStore::DependsOnLocked(const Entry& entry, const std::string& response_id) const {
  const Entry* current = &entry;
  std::unordered_set<std::string> visited;

  while (current != nullptr && visited.insert(current->id).second) {
    if (current->id == response_id || PrefixContains(current->replay_prefix, response_id)) {
      return true;
    }

    const auto previous = current->response.find("previous_response_id");
    if (previous == current->response.end() || !previous->is_string() || previous->get<std::string>().empty()) {
      break;
    }

    const auto parent = index_.find(previous->get<std::string>());
    current = parent == index_.end() ? nullptr : &*parent->second;
  }

  return false;
}

ResponseChainHop ResponseStore::ToReplayHop(const Entry& entry) {
  ResponseChainHop replay;
  if (entry.input_items.is_array()) {
    replay.input_items = entry.input_items;
  }

  const auto output = entry.response.find("output");
  if (output != entry.response.end() && output->is_array()) {
    replay.output_items = *output;
  }
  replay.output_tool_kinds = entry.output_tool_kinds;

  return replay;
}

std::unordered_set<std::string> ResponseStore::ChainIdsLocked(const WalkedChain& chain) {
  std::unordered_set<std::string> ids;
  for (const auto& hop : chain.hops) {
    ids.insert(hop->id);
  }

  for (auto node = chain.replay_prefix.get(); node != nullptr; node = node->previous.get()) {
    ids.insert(node->id);
  }

  return ids;
}

std::shared_ptr<const ResponseStore::ReplayPrefix> ResponseStore::SnapshotChainLocked(const WalkedChain& chain) {
  auto snapshot = chain.replay_prefix;
  for (auto hop = chain.hops.rbegin(); hop != chain.hops.rend(); ++hop) {
    snapshot = std::make_shared<ReplayPrefix>(ReplayPrefix{(*hop)->id, std::move(snapshot), ToReplayHop(**hop)});
  }

  return snapshot;
}

void ResponseStore::TouchLocked(std::list<Entry>::iterator it) {
  if (it != entries_.begin()) {
    entries_.splice(entries_.begin(), entries_, it);
  }
}

}  // namespace fl
