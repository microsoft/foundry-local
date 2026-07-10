// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "logger.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fl {

class ICatalog;
class ITelemetry;
class ModelLoadManager;
class SessionManager;
class ResponseStore;

/// Tracks streaming threads so they can be joined on shutdown.
/// Handlers call Track() instead of std::thread::detach().
class StreamingThreadTracker {
  struct TrackedThread {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
    std::function<void()> abort;
  };

 public:
  /// Take ownership of a streaming thread.
  void Track(std::thread t, std::shared_ptr<std::atomic<bool>> done, std::function<void()> abort) {
    std::lock_guard<std::mutex> lock(mutex_);
    ReapCompletedLocked();
    threads_.push_back(TrackedThread{std::move(t), std::move(done), std::move(abort)});
  }

  void AbortAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& tracked : threads_) {
      if (tracked.abort) {
        tracked.abort();
      }
    }
  }

  /// Join all remaining threads. Called by WebService::Stop().
  /// Moves entries out before joining so completed streaming threads are cleaned up safely.
  void JoinAll() {
    std::vector<TrackedThread> local;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      local = std::move(threads_);
    }

    for (auto& tracked : local) {
      if (tracked.thread.joinable()) {
        tracked.thread.join();
      }
    }
  }

 private:
  void ReapCompletedLocked() {
    for (auto it = threads_.begin(); it != threads_.end();) {
      if (it->done && it->done->load(std::memory_order_acquire)) {
        if (it->thread.joinable()) {
          it->thread.join();
        }
        it = threads_.erase(it);
      } else {
        ++it;
      }
    }
  }

  std::mutex mutex_;
  std::vector<TrackedThread> threads_;
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
/// Creates and owns ResponseStore, StreamingThreadTracker, and ServiceContext internally.
class WebService {
 public:
  WebService(ICatalog& catalog, ILogger& logger, std::string model_cache_dir,
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
