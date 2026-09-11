// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/session_manager.h"

#include "exception.h"
#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/session/session.h"
#include "util/scope_guard.h"

#include <algorithm>
#include <cassert>
#include <fmt/format.h>

namespace fl {

SessionManager::SessionManager(ILogger& logger, size_t cache_capacity)
    : logger_(logger), cache_capacity_(std::max(cache_capacity, size_t{1})) {
}

SessionManager::~SessionManager() {
  // Clear cache first — destroying cached sessions frees resources.
  ClearCache();

  WaitForDrain();
}

void SessionManager::Register(Session& session) {
  // Check shutting_down_ and insert under the same lock CancelAll() uses to flip the flag and iterate.
  // Reading the flag outside the lock would let a session observe false, lose the race to the cancel
  // sweep, then insert itself afterward — running uncanceled while shutdown waits to drain.
  std::lock_guard<std::mutex> lock(mutex_);

  if (shutting_down_.load()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot create session during shutdown");
  }

  sessions_.insert(&session);
}

void SessionManager::Deregister(Session& session) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto erased = sessions_.erase(&session);

  if (erased == 0) {
    // Bug: session was not registered. Log loudly but don't throw — this may be
    // called from a destructor where throwing would call std::terminate().
    logger_.Log(LogLevel::Error, "SessionManager::Deregister called for unregistered session");
    assert(false && "SessionManager::Deregister called for unregistered session");
    return;
  }

  if (sessions_.empty()) {
    drain_cv_.notify_all();
  }
}

void SessionManager::CancelAll() {
  // Clear cache first — frees idle cached sessions so they don't block drain. ClearCache() takes mutex_
  // internally, so run it before acquiring the lock below.
  ClearCache();

  std::lock_guard<std::mutex> lock(mutex_);

  // Flip the flag under mutex_ before iterating so Register() (which now checks it under the same lock)
  // cannot admit a new session between this transition and the cancel sweep.
  shutting_down_.store(true);

  logger_.Log(LogLevel::Information,
              fmt::format("SessionManager: cancelling all sessions ({} active)", sessions_.size()));

  // Signal every in-flight request to stop. Cancel() only sets atomic flags — no joins, no
  // re-entrancy into SessionManager — so calling it while holding mutex_ cannot deadlock.
  for (Session* s : sessions_) {
    s->Cancel();
  }
}

void SessionManager::WaitForDrain(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);

  if (sessions_.empty()) {
    return;
  }

  logger_.Log(LogLevel::Information,
              fmt::format("SessionManager: waiting for {} active sessions to drain", sessions_.size()));

  bool drained = drain_cv_.wait_for(lock, timeout, [this] { return sessions_.empty(); });

  if (!drained) {
    logger_.Log(LogLevel::Warning,
                fmt::format("SessionManager: drain timed out with {} sessions still active", sessions_.size()));
  }
}

size_t SessionManager::ActiveCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.size();
}

// --- Session cache ---

std::unique_ptr<ChatSession> SessionManager::CheckOut(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = cache_.find(key);

  if (it == cache_.end()) {
    return nullptr;
  }

  auto session = std::move(it->second.session);
  lru_order_.erase(it->second.lru_iter);
  cache_.erase(it);

  logger_.Log(LogLevel::Debug, fmt::format("SessionManager: checked out cached session for '{}'", key));
  return session;
}

void SessionManager::CheckIn(const std::string& key, std::unique_ptr<ChatSession> session) {
  std::unique_ptr<ChatSession> displaced;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    auto existing = cache_.find(key);
    const auto committed_size = existing == cache_.end() ? std::min(cache_.size() + 1, cache_capacity_)
                                                         : cache_.size();
    logger_.Log(LogLevel::Debug,
                fmt::format("SessionManager: checking in session under '{}' (cache size: {})", key,
                            committed_size));

    // Allocate every node before changing or evicting an existing entry. If insertion throws, removing this new list
    // node restores the exact prior cache state.
    lru_order_.push_front(key);
    auto rollback_lru = ScopeGuard([this]() noexcept { lru_order_.pop_front(); });

    if (existing != cache_.end()) {
      displaced = std::move(existing->second.session);
      existing->second.session = std::move(session);
      const auto old_lru = existing->second.lru_iter;
      existing->second.lru_iter = lru_order_.begin();
      lru_order_.erase(old_lru);
      rollback_lru.Dismiss();
    } else {
      auto inserted = cache_.try_emplace(key, CacheEntry{nullptr, lru_order_.begin()}).first;
      inserted->second.session = std::move(session);
      rollback_lru.Dismiss();

      if (cache_.size() > cache_capacity_) {
        const auto& lru_key = lru_order_.back();
        auto lru = cache_.find(lru_key);
        assert(lru != cache_.end());
        displaced = std::move(lru->second.session);
        cache_.erase(lru);
        lru_order_.pop_back();
      }
    }

    assert(cache_.size() <= cache_capacity_);
  }
}

size_t SessionManager::CacheSize() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cache_.size();
}

bool SessionManager::EvictCached(const std::string& key) noexcept {
  // Destroy outside the lock: ~ChatSession calls Deregister which re-acquires mutex_.
  std::unique_ptr<ChatSession> evicted;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(key);

    if (it == cache_.end()) {
      return false;
    }

    evicted = std::move(it->second.session);
    lru_order_.erase(it->second.lru_iter);
    cache_.erase(it);
  }

  try {
    logger_.Log(LogLevel::Debug, fmt::format("SessionManager: evicted cached session for '{}'", key));
  } catch (...) {
    // Cache coordination is part of ResponseStore's no-throw publication phase. Diagnostic logging cannot roll back
    // the removal and must not break metadata/session coherence.
  }

  return true;
}

void SessionManager::ClearCache() {
  std::vector<std::unique_ptr<ChatSession>> to_destroy;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [key, entry] : cache_) {
      to_destroy.push_back(std::move(entry.session));
    }

    cache_.clear();
    lru_order_.clear();
  }

  // Destroy outside lock
}

}  // namespace fl
