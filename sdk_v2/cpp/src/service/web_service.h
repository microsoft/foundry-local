// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "logger.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace fl {

class ICatalog;
class ITelemetry;
class ModelLoadManager;
class SessionManager;
class ResponseStore;

/// Tracks streaming threads so they can be joined on shutdown.
class StreamingThreadTracker {
 public:
  /// Register before the worker can untrack itself; release its captures before declaring it finished.
  template <typename Work>
  void Start(Work&& work) {
    std::lock_guard<std::mutex> lock(mutex_);
    threads_.emplace_back([this, task = std::optional<std::decay_t<Work>>(std::forward<Work>(work))]() mutable {
      (*task)();
      task.reset();
      Remove(std::this_thread::get_id());
    });
  }

  std::size_t TrackedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return threads_.size();
  }

  /// Join all remaining threads. Called by WebService::Stop().
  /// Moves entries out before joining to avoid deadlock with Remove().
  void JoinAll() {
    std::vector<std::thread> local;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      local = std::move(threads_);
    }

    for (auto& t : local) {
      if (t.joinable()) {
        t.join();
      }
    }
  }

 private:
  void Remove(std::thread::id id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = threads_.begin(); it != threads_.end(); ++it) {
      if (it->get_id() == id) {
        it->detach();
        threads_.erase(it);
        return;
      }
    }
  }

  mutable std::mutex mutex_;
  std::vector<std::thread> threads_;
};

/// Context shared with all HTTP controllers.
/// Provides access to the manager's internal state without exposing flManager directly.
struct ServiceContext {
  ICatalog& catalog;
  ILogger& logger;
  std::string model_cache_dir;
  std::vector<std::string> bound_urls;
  ModelLoadManager& model_load_manager;
  SessionManager& session_manager;
  ResponseStore& response_store;
  ITelemetry& telemetry;
  StreamingThreadTracker& thread_tracker;
};

/// HTTP web service wrapping oatpp.
/// Lifetime:
///   1. Construct with required dependencies
///   2. Start(endpoints) — binds to addresses, launches listener threads
///   3. Stop() — graceful shutdown
///
/// Creates and owns the response stores, StreamingThreadTracker, and catalog-bound ServiceContexts internally.
class WebService {
 public:
  WebService(ICatalog& public_catalog, ICatalog& local_catalog, ILogger& logger, std::string model_cache_dir,
             ModelLoadManager& model_load_manager, SessionManager& session_manager,
             ITelemetry& telemetry, std::function<void()> shutdown_callback);
  ~WebService();

  WebService(const WebService&) = delete;
  WebService& operator=(const WebService&) = delete;

  /// Start the HTTP service on the given endpoints.
  /// Each endpoint string is a URL like "http://127.0.0.1:8080" or "http://127.0.0.1:0" for ephemeral.
  /// Returns the actual bound URLs (with resolved ports).
  std::vector<std::string> Start(const std::vector<std::string>& endpoints);

  /// Stop the HTTP service and join listener threads.
  void Stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fl
