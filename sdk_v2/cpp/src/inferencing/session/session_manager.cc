// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/session_manager.h"

#include "exception.h"
#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/session/session.h"

#include <cassert>
#include <fmt/format.h>
#include <vector>

namespace fl {

SessionManager::SessionManager(ILogger& logger, size_t cache_capacity)
    : logger_(logger), cache_capacity_(cache_capacity) {
}

SessionManager::~SessionManager() {
  // Clear cache first — destroying cached sessions frees resources.
  ClearCache();

  WaitForDrain();
}

void SessionManager::Register(Session& session) {
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
  shutting_down_.store(true);

  // Clear cache — frees idle cached sessions so they don't block drain.
  ClearCache();

  std::vector<Session*> sessions;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions.assign(sessions_.begin(), sessions_.end());
  }

  logger_.Log(LogLevel::Information,
              fmt::format("SessionManager: cancelling all sessions ({} active)", sessions.size()));

  for (auto* session : sessions) {
    if (session != nullptr) {
      session->Cancel();
    }
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
  // Collect evicted sessions to destroy outside the lock.
  std::vector<std::unique_ptr<ChatSession>> evicted;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    // Replace existing entry for this key (if any)
    auto existing = cache_.find(key);
    if (existing != cache_.end()) {
      evicted.push_back(std::move(existing->second.session));
      lru_order_.erase(existing->second.lru_iter);
      cache_.erase(existing);
    }

    // Evict LRU if at capacity
    while (cache_.size() >= cache_capacity_) {
      const auto& lru_key = lru_order_.back();
      auto lru_it = cache_.find(lru_key);
      evicted.push_back(std::move(lru_it->second.session));
      cache_.erase(lru_it);
      lru_order_.pop_back();
    }

    // Insert new entry
    lru_order_.push_front(key);
    cache_[key] = CacheEntry{std::move(session), lru_order_.begin()};

    logger_.Log(LogLevel::Debug,
                fmt::format("SessionManager: checked in session under '{}' (cache size: {})", key, cache_.size()));
  }

  // Evicted sessions destroyed here, outside lock
  if (!evicted.empty()) {
    logger_.Log(LogLevel::Debug,
                fmt::format("SessionManager: evicted {} cached session(s)", evicted.size()));
  }
}

size_t SessionManager::CacheSize() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cache_.size();
}

bool SessionManager::EvictCached(const std::string& key) {
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

  logger_.Log(LogLevel::Debug, fmt::format("SessionManager: evicted cached session for '{}'", key));
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
