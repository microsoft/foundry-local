// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "configuration.h"
#include "ep_detection/ep_detector.h"
#include "logger.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct OrtApi;
struct OrtEnv;

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
namespace fl {
class WebService;
}  // namespace fl
#endif

namespace fl {

// Forward declarations
class ICatalog;
class DownloadManager;
class ITelemetry;
class Model;
class ModelLoadManager;
class SessionManager;
struct ModelInfo;

/// Central manager for the Foundry Local SDK.
class Manager {
 public:
  /// Create and initialize the singleton manager.
  /// The manager creates its own logger from the configuration.
  static Manager& Create(const Configuration& config);

  /// Access the existing singleton. Throws if not yet created.
  static Manager& Instance();

  /// Destroy the singleton and release all resources.
  static void Destroy();

  /// Get the shared catalog interface for querying models.
  /// The catalog is owned by the manager and shared across all consumers
  /// (web service, C API, etc.) so model state (e.g. IsLoaded) is consistent.
  ICatalog& GetCatalog();

  /// Get the configuration used to create this manager.
  const Configuration& GetConfiguration() const;

  /// Get the download manager (for advanced usage or testing).
  DownloadManager& GetDownloadManager();

  /// Get the EP detector for hardware/device detection.
  const IEpDetector& GetEpDetector() const;

  /// Get the EP detector (non-const for download operations).
  IEpDetector& GetEpDetector();

  /// Download and register EPs, and invalidate the catalog cache on success
  /// so subsequent catalog queries reflect the new device/EP availability.
  /// This is the preferred entry point — going through GetEpDetector() directly
  /// will not invalidate the catalog.
  EpDownloadResult DownloadAndRegisterEps(
      const std::vector<std::string>* names,
      const IEpBootstrapper::ProgressCallback& progress_cb);

  /// Get the model load manager (for loading/unloading ORT GenAI models).
  ModelLoadManager& GetModelLoadManager();

  /// Get the session manager for tracking session lifecycle.
  SessionManager& GetSessionManager();

  /// Start the embedded web service. Binds to configured endpoints.
  void StartWebService();

  /// Get the bound service URLs. The returned reference is valid as long as
  /// the web service is running. Throws if web service is not running.
  const std::vector<std::string>& GetWebServiceUrls() const;

  /// Stop the embedded web service.
  void StopWebService();

  /// Begin graceful shutdown. Sets the shutdown flag and signals subsystems to drain.
  /// Idempotent and non-blocking — safe to call from any thread including web service handlers.
  /// Destroy() calls this internally, so direct SDK users don't need to call it explicitly.
  void Shutdown();

  /// Check if Shutdown() has been called (from any source — web endpoint, signal, user code).
  bool IsShutdownRequested() const;

  /// Get the logger instance.
  ILogger& GetLogger();

  /// Get the telemetry interface.
  ITelemetry& GetTelemetry();

  // Non-copyable, non-movable (singleton)
  Manager(const Manager&) = delete;
  Manager& operator=(const Manager&) = delete;

 private:
  Manager(const Configuration& config);
  ~Manager();

  // unique_ptr<Manager> needs access to the private destructor for cleanup.
  friend struct std::default_delete<Manager>;

  // Member ordering matters: C++ destroys in reverse declaration order.
  // Providers (destroyed last) must be declared before consumers (destroyed first).
  //
  //   config_                  — trivial data, no dependencies
  //   ort_api_, ort_env_,
  //     registered_ep_libraries_ — ORT environment & EP registrations;
  //                                released manually in ~Manager() after all
  //                                consumers (sessions, ep_detector_) are gone.
  //   logger_                  — everything logs through this, destroyed last
  //   telemetry_               — used by ep_detector_ and throughout; must
  //                              outlive ep_detector_ because EpDetector holds
  //                              a raw ITelemetry* for emitting EP events
  //   ep_detector_             — detects HW acceleration; holds OrtEnv& (must
  //                              outlive ort_env_ release in ~Manager()) and
  //                              a raw ITelemetry*
  //   catalog_                 — owns all Model instances. used by download_manager, model_load_manager, and web service
  //   download_manager_        — uses ModelInfo owned by catalog
  //   model_load_manager_      — holds loaded model state referencing catalog models
  //   session_manager_         — tracks all active sessions. destroyed after web service, before models
  //   shutdown_requested_      — atomic flag checked by subsystems and the host process
  //   web service members      — use catalog, model_load_manager, session_manager, telemetry, logger
  //
  Configuration config_;
  const OrtApi* ort_api_ = nullptr;
  OrtEnv* ort_env_ = nullptr;
  std::vector<std::string> registered_ep_libraries_;
  std::unique_ptr<ILogger> logger_;
  std::unique_ptr<ITelemetry> telemetry_;
  std::unique_ptr<IEpDetector> ep_detector_;
  std::unique_ptr<ICatalog> catalog_;
  std::unique_ptr<DownloadManager> download_manager_;
  std::unique_ptr<ModelLoadManager> model_load_manager_;
  std::unique_ptr<SessionManager> session_manager_;
  std::atomic<bool> shutdown_requested_{false};
  std::atomic<bool> web_service_running_{false};
  std::vector<std::string> bound_urls_;

#ifdef FOUNDRY_LOCAL_HAS_WEB_SERVICE
  std::unique_ptr<WebService> web_service_;
#endif

 private:
  Model CreateModel(ModelInfo info, std::string local_path);

  static std::mutex s_mutex_;
  static std::unique_ptr<Manager> s_instance_;
};

}  // namespace fl
