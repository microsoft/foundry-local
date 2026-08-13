// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/session_manager.h"

#include "exception.h"
#include "inferencing/generative/chat/chat_session.h"
#include "inferencing/session/live_session_registry.h"
#include "inferencing/session/session.h"
#include "inferencing/session/session_runtime.h"

#include <cassert>
#include <vector>

#include <fmt/format.h>

namespace fl {

SessionManager::SessionManager(ILogger& logger, size_t cache_capacity)
    : logger_(logger), cache_capacity_(cache_capacity) {
  // Deliberately does not touch the process-wide admission gate. The registry is a leaked singleton shared
  // across Manager lifetimes; a new Manager must never reopen a closure another Manager still owns.
}

SessionManager::~SessionManager() {
  // Clear cache first — destroying cached sessions frees resources.
  ClearCache();

  WaitForDrain();

  // admission_closure_ is released by its own destructor, after the drain above: this Manager's shutdown is
  // the only thing it ever held closed, so a sequentially recreated Manager admits new sessions again.
}

void SessionManager::Register(Session& session) {
  // Track the stable runtime, not the facade address: handlers move sessions into background threads, and a
  // moved facade must not look like a different (or a vanished) registration.
  const auto runtime = session.Runtime();
  if (!runtime) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot register a moved-from session");
  }

  // Check shutdown and insert under the same lock so a registration cannot slip past the manager gate.
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutting_down_.load()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "cannot create session during shutdown");
  }

  sessions_.insert(runtime.get());
}

void SessionManager::Deregister(Session& session) {
  SessionRuntime* runtime = session.Runtime().get();
  bool missing = false;
  bool drained = false;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto erased = runtime != nullptr ? sessions_.erase(runtime) : 0;
    missing = erased == 0;
    drained = sessions_.empty();
  }

  if (missing) {
    // Bug: session was not registered. Log loudly but don't throw — this may be
    // called from a destructor where throwing would call std::terminate().
    logger_.Log(LogLevel::Error, "SessionManager::Deregister called for unregistered session");
    assert(false && "SessionManager::Deregister called for unregistered session");
    return;
  }

  if (drained) {
    drain_cv_.notify_all();
  }
}

void SessionManager::CancelAll() {
  {
    // Close both manager and process-wide admission before clearing the cache or taking the live snapshot.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!shutting_down_.exchange(true)) {
      admission_closure_ = LiveSessionRegistry::Instance().CloseAdmission();
    }
  }

  // Clear cache after closing admission so a concurrent check-in cannot repopulate it.
  ClearCache();

  // Cancel every live session runtime, not just registered ones: direct-API sessions never take a
  // SessionRegistration, yet a runaway request on one is exactly what pins the model.
  //
  // Snapshot promotes the weak entries to strong ones and cancels outside any lock — CancelAll() reaches
  // into the ORT GenAI engine, and a cancelled session unwinding calls back into Deregister(). Holding
  // strong runtime references also means a facade released concurrently cannot pull the runtime out from
  // under this loop.
  std::vector<std::shared_ptr<SessionRuntime>> to_cancel = LiveSessionRegistry::Instance().Snapshot();

  logger_.Log(LogLevel::Information,
              fmt::format("SessionManager: cancelling all sessions ({} live)", to_cancel.size()));

  for (const auto& control : to_cancel) {
    control->CancelAll();
  }
}

void SessionManager::WaitForDrain(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);

  if (sessions_.empty()) {
    return;
  }

  const auto initial_count = sessions_.size();
  lock.unlock();
  logger_.Log(LogLevel::Information,
              fmt::format("SessionManager: waiting for {} active sessions to drain", initial_count));

  lock.lock();
  const bool drained = drain_cv_.wait_for(lock, timeout, [this] { return sessions_.empty(); });
  const auto remaining = sessions_.size();
  lock.unlock();

  if (!drained) {
    logger_.Log(LogLevel::Warning,
                fmt::format("SessionManager: drain timed out with {} sessions still active", remaining));
  }
}

size_t SessionManager::ActiveCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.size();
}

// --- Session cache ---

std::unique_ptr<ChatSession> SessionManager::CheckOut(const std::string& key) {
  std::unique_ptr<ChatSession> session;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(key);

    if (it == cache_.end()) {
      return nullptr;
    }

    session = std::move(it->second.session);
    lru_order_.erase(it->second.lru_iter);
    cache_.erase(it);
  }

  logger_.Log(LogLevel::Debug, fmt::format("SessionManager: checked out cached session for '{}'", key));
  return session;
}

void SessionManager::CheckIn(const std::string& key, std::unique_ptr<ChatSession> session) {
  // Collect evicted sessions to destroy outside the lock.
  std::vector<std::unique_ptr<ChatSession>> evicted;
  bool discarded = false;
  size_t cache_size = 0;

  {
    std::lock_guard<std::mutex> lock(mutex_);

    // A response handler may finish while shutdown is cancelling live work. Do not let it repopulate the cache after
    // CancelAll() has cleared it; cached sessions are deregistered and therefore invisible to WaitForDrain().
    if (shutting_down_.load()) {
      discarded = true;
    } else {
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
      cache_size = cache_.size();
    }
  }

  if (discarded) {
    logger_.Log(LogLevel::Debug, "SessionManager: discarding session checked in during shutdown");
    return;
  }

  logger_.Log(LogLevel::Debug,
              fmt::format("SessionManager: checked in session under '{}' (cache size: {})", key, cache_size));

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
  // Destroy outside the lock: facade destruction cancels its operations and may run operation cleanup.
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
