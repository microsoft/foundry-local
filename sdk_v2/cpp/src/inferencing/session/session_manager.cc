// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/session_manager.h"

#include "exception.h"
#include "inferencing/generative/chat/chat_session.h"

#include <cassert>
#include <fmt/format.h>

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
  if (shutting_down_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot create session during shutdown");
  }

  ++sessions_[&session];
}

void SessionManager::Deregister(Session& session) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = sessions_.find(&session);

  if (it == sessions_.end()) {
    // Bug: session was not registered. Log loudly but don't throw — this may be
    // called from a destructor where throwing would call std::terminate().
    logger_.Log(LogLevel::Error, "SessionManager::Deregister called for unregistered session");
    assert(false && "SessionManager::Deregister called for unregistered session");
    return;
  }

  if (--it->second == 0) {
    sessions_.erase(it);
  }

  if (sessions_.empty()) {
    drain_cv_.notify_all();
  }
}

void SessionManager::CancelAll() {
  std::vector<std::unique_ptr<ChatSession>> cached_sessions;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutting_down_ = true;
    cached_sessions = MoveCacheToDestroyLocked();
    logger_.Log(LogLevel::Information,
                fmt::format("SessionManager: cancelling all sessions ({} active)", sessions_.size()));

    for (const auto& session_entry : sessions_) {
      session_entry.first->Cancel();
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

  auto session = RemoveCachedLocked(it);

  logger_.Log(LogLevel::Debug, fmt::format("SessionManager: checked out cached session for '{}'", key));
  return session;
}

void SessionManager::CheckIn(const std::string& key, std::unique_ptr<ChatSession> session) {
  // Collect evicted sessions to destroy outside the lock.
  std::vector<std::unique_ptr<ChatSession>> evicted;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_ || cache_capacity_ == 0) {
      evicted.push_back(std::move(session));
      return;
    }

    auto existing = cache_.find(key);
    if (existing != cache_.end()) {
      evicted.push_back(RemoveCachedLocked(existing));
    }

    while (cache_.size() >= cache_capacity_) {
      const auto& lru_key = lru_order_.back();
      auto lru_it = cache_.find(lru_key);
      evicted.push_back(RemoveCachedLocked(lru_it));
    }

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

    evicted = RemoveCachedLocked(it);
  }

  logger_.Log(LogLevel::Debug, fmt::format("SessionManager: evicted cached session for '{}'", key));
  return true;
}

std::unique_ptr<ChatSession> SessionManager::RemoveCachedLocked(CacheMap::iterator it) {
  auto session = std::move(it->second.session);
  lru_order_.erase(it->second.lru_iter);
  cache_.erase(it);
  return session;
}

std::vector<std::unique_ptr<ChatSession>> SessionManager::MoveCacheToDestroyLocked() {
  std::vector<std::unique_ptr<ChatSession>> to_destroy;

  for (auto& cache_entry : cache_) {
    to_destroy.push_back(std::move(cache_entry.second.session));
  }

  cache_.clear();
  lru_order_.clear();
  return to_destroy;
}

void SessionManager::ClearCache() {
  std::vector<std::unique_ptr<ChatSession>> to_destroy;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    to_destroy = MoveCacheToDestroyLocked();
  }
}

}  // namespace fl
