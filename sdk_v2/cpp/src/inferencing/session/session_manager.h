// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "logger.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fl {

class ChatSession;
class Session;

/// Interface for session lifecycle tracking.
/// Session depends on this — not the concrete SessionManager.
class ISessionManager {
 public:
  virtual ~ISessionManager() = default;
  virtual void Register(Session& session) = 0;
  virtual void Deregister(Session& session) = 0;
};

/// Tracks all active sessions and caches Responses API sessions for KV cache reuse.
/// Owned by Manager. Destroyed after web service, before model load manager.
///
/// Registration is external via SessionRegistration RAII guard — Session itself
/// does not know about SessionManager.
///
/// Cache semantics (check-out / check-in):
/// - CheckOut(key) removes the session from the cache and transfers ownership to the caller.
///   While checked out, the session cannot be evicted or accessed by concurrent requests.
/// - CheckIn(key, session) inserts the session into the cache under the given key.
///   May evict the least-recently-used entry if at capacity.
///
/// Cached sessions are NOT registered — they are idle. Only sessions actively
/// processing requests should be registered via SessionRegistration.
class SessionManager : public ISessionManager {
 public:
  static constexpr size_t kDefaultCacheCapacity = 5;

  explicit SessionManager(ILogger& logger, size_t cache_capacity = kDefaultCacheCapacity);
  ~SessionManager() override;

  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;

  /// Register a session. Called by SessionRegistration constructor.
  /// Throws if shutting down.
  void Register(Session& session) override;

  /// Deregister a session. Called by SessionRegistration destructor.
  /// Logs an error (but does not throw) if the session was not registered.
  void Deregister(Session& session) override;

  /// Signal all sessions to stop and reject new registrations.
  /// Clears the cache (destroying idle cached sessions).
  ///
  /// Cancellation targets every live Session in the process (see LiveSessionRegistry),
  /// not just registered ones: a runaway request on a direct-API session never goes
  /// through SessionRegistration, yet it is exactly what pins the model loaded.
  void CancelAll();

  /// Block until all sessions have deregistered, with timeout.
  void WaitForDrain(std::chrono::milliseconds timeout = std::chrono::seconds(10));

  /// Number of currently tracked sessions (active + cached).
  size_t ActiveCount() const;

  // --- Session cache (Responses API) ---

  /// Remove a session from the cache by key and return it.
  /// Returns nullptr on cache miss. The session is still tracked (registered).
  std::unique_ptr<ChatSession> CheckOut(const std::string& key);

  /// Insert a session into the cache under the given key.
  /// May evict the least-recently-used entry if at capacity.
  /// The session remains tracked (registered) while cached.
  void CheckIn(const std::string& key, std::unique_ptr<ChatSession> session);

  /// Remove and destroy a cached session by key. Returns true if an entry was removed.
  ///
  /// Used by the DELETE /v1/responses/{id} path: the response store entry is gone, so
  /// the cached session must also go — otherwise it pins the model loaded and a
  /// subsequent client-side unload fails with "session(s) still using it".
  bool EvictCached(const std::string& key);

  /// Number of sessions currently in the cache.
  size_t CacheSize() const;

 private:
  struct CacheEntry {
    std::unique_ptr<ChatSession> session;
    std::list<std::string>::iterator lru_iter;
  };

  /// Clear all cache entries. Acquires mutex_ to move entries out, then destroys
  /// them outside the lock (destroyed sessions call Deregister, which acquires mutex_).
  void ClearCache();

  ILogger& logger_;
  size_t cache_capacity_;
  std::atomic<bool> shutting_down_{false};
  mutable std::mutex mutex_;
  std::condition_variable drain_cv_;
  std::unordered_set<Session*> sessions_;

  // LRU cache: front of lru_order_ = most recently used
  std::list<std::string> lru_order_;
  std::unordered_map<std::string, CacheEntry> cache_;
};

}  // namespace fl
