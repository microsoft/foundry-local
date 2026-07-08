// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "download/download_manager.h"
#include "download/cross_process_file_lock.h"
#include "download/inference_model_writer.h"
#include "exception.h"
#include "log_level.h"
#include "logger.h"
#include "util/path_safety.h"
#include "util/region_fallback.h"
#include "utils.h"

#include <foundry_local/foundry_local_c.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace fl {

namespace {

const char* kDownloadSignalFileName = "download.tmp";
const char* kGenAIConfigFileName = "genai_config.json";
const char* kInferenceModelFileName = "inference_model.json";
const char* kDefaultRegistryRegion = "centralus";

/// Check whether inference_model.json exists at the root or in any immediate
/// subdirectory.  This is the definitive proof that a download completed
/// successfully — DownloadModel writes it in Step 3.
bool HasInferenceModelJson(const std::string& model_path) {
  if (std::filesystem::exists(std::filesystem::path(model_path) / kInferenceModelFileName)) {
    return true;
  }

  std::error_code ec;
  std::filesystem::directory_iterator it(model_path, ec);
  if (ec) {
    return false;
  }

  for (const auto& entry : it) {
    if (entry.is_directory(ec)) {
      if (std::filesystem::exists(entry.path() / kInferenceModelFileName)) {
        return true;
      }
    }
  }

  return false;
}

/// Resolve the effective model path — the directory containing genai_config.json.
/// For single-variant models this is model_path itself.
/// For multi-variant models it's the first subdirectory containing genai_config.json.
std::string ResolveEffectiveModelPath(const std::string& model_path) {
  auto root_config = std::filesystem::path(model_path) / kGenAIConfigFileName;
  if (std::filesystem::exists(root_config)) {
    return model_path;
  }

  std::error_code ec;
  std::filesystem::directory_iterator it(model_path, ec);
  if (ec) {
    return model_path;
  }

  for (const auto& entry : it) {
    if (entry.is_directory(ec)) {
      auto sub_config = entry.path() / kGenAIConfigFileName;
      if (std::filesystem::exists(sub_config)) {
        return entry.path().string();
      }
    }
  }

  // No genai_config.json found — return root
  return model_path;
}

/// Convert ':<version>' suffix to '-<version>' for filesystem compatibility.
/// Matches C# AzureFoundryLocalDownloadClientProvider.GetModelDirectoryName.
std::string FixVersionSuffix(const std::string& name) {
  auto last_colon = name.rfind(':');
  if (last_colon == std::string::npos || last_colon == 0) {
    return name;
  }

  // Check that what follows the colon is a number
  auto suffix = name.substr(last_colon + 1);
  bool all_digits = !suffix.empty();
  for (char c : suffix) {
    if (c < '0' || c > '9') {
      all_digits = false;
      break;
    }
  }

  if (!all_digits) {
    return name;
  }

  return name.substr(0, last_colon) + "-" + suffix;
}

/// Reject any string that, if used as a single path segment, could escape the cache root or
/// produce a Windows-reserved/ambiguous path. The catalog is HTTPS-fetched from MS-controlled
/// servers, so this is defense-in-depth — we never trust untrusted bytes to address the
/// filesystem directly.
std::string SanitizeForPathSegment(std::string_view name) {
  if (name.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "path segment is empty");
  }

  // Whitespace at either end and a trailing '.' are silently stripped by Windows when
  // resolving paths, which can cause surprising aliasing. Reject outright.
  auto first = static_cast<unsigned char>(name.front());
  auto last = static_cast<unsigned char>(name.back());
  if (std::isspace(first) || std::isspace(last) || name.front() == '.' || name.back() == '.') {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "path segment has leading/trailing whitespace or '.': '" + std::string(name) + "'");
  }

  for (char c : name) {
    if (c == '\0') {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "path segment contains null byte");
    }

    // Reject any path separator or Windows drive/stream marker so the value cannot
    // expand into multiple components or address an unrelated volume.
    if (c == '/' || c == '\\' || c == ':') {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
               std::string("path segment contains forbidden character '") + c + "': '" +
                   std::string(name) + "'");
    }
  }

  // Reject `..` (and any segment that is purely dots) — even after the separator check
  // above, a literal ".." would still resolve as parent on every filesystem.
  bool only_dots = true;
  for (char c : name) {
    if (c != '.') {
      only_dots = false;
      break;
    }
  }
  if (only_dots) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "path segment is reserved ('.' or '..'): '" + std::string(name) + "'");
  }

  return std::string(name);
}

std::string NormalizeConfiguredRegion(std::string_view catalog_region) {
  const auto normalized = ToLower(std::string(catalog_region));
  return normalized == "auto" ? "" : normalized;
}

std::string ResolveRegion(const std::string& config_region, const ModelInfo& info) {
  // Explicit configured region always wins; then the model's detected region; then the default registry region.
  if (!config_region.empty()) {
    return config_region;
  }

  if (!info.detected_region.empty()) {
    return info.detected_region;
  }

  return kDefaultRegistryRegion;
}

}  // anonymous namespace

DownloadManager::DownloadManager(std::string cache_directory, std::string_view catalog_region, int max_concurrency,
                                 ILogger& logger, bool disable_region_fallback)
    : cache_directory_(std::move(cache_directory)),
      config_region_(NormalizeConfiguredRegion(catalog_region)),
      max_concurrency_(max_concurrency),
      logger_(logger),
      registry_client_(std::make_unique<ModelRegistryClient>(
          kDefaultRegistryRegion, logger, std::make_unique<RegionFallback>(logger, !disable_region_fallback))),
      blob_downloader_(std::make_unique<AzureBlobDownloader>(logger)) {}

DownloadManager::~DownloadManager() = default;

void DownloadManager::SetModelRegistryClient(std::unique_ptr<ModelRegistryClient> client) {
  registry_client_ = std::move(client);
}

void DownloadManager::SetBlobDownloader(std::unique_ptr<IBlobDownloader> downloader) {
  blob_downloader_ = std::move(downloader);
}

std::string DownloadManager::ComputeModelPath(const ModelInfo& info) const {
  // Get publisher from string properties
  std::string publisher;
  auto it = info.string_properties.find(FOUNDRY_LOCAL_MODEL_PROP_PUBLISHER_STR);
  if (it != info.string_properties.end()) {
    publisher = it->second;
  }

  // Sanitize each component separately so untrusted catalog input cannot inject path
  // separators, '..', drive letters, or trailing dots that would let it escape
  // cache_directory_ or alias another model.
  // model_id format is "name:version" — split, sanitize each piece, then re-join via
  // FixVersionSuffix so the on-disk layout is unchanged for valid inputs.
  std::string sanitized_model_dir;
  auto last_colon = info.model_id.rfind(':');
  if (last_colon == std::string::npos || last_colon == 0) {
    sanitized_model_dir = SanitizeForPathSegment(info.model_id);
  } else {
    auto bare_id = std::string_view(info.model_id).substr(0, last_colon);
    auto version = std::string_view(info.model_id).substr(last_colon + 1);
    SanitizeForPathSegment(bare_id);
    SanitizeForPathSegment(version);
    sanitized_model_dir = FixVersionSuffix(info.model_id);
  }

  std::filesystem::path full_path(cache_directory_);
  if (!publisher.empty()) {
    full_path /= SanitizeForPathSegment(publisher);
  }
  full_path /= sanitized_model_dir;

  // Final defense in depth: even after segment-level sanitization, confirm the
  // resolved path is still inside the cache directory.
  if (!IsPathWithinDirectory(full_path, std::filesystem::path(cache_directory_))) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
             "computed model path escapes cache directory: " + full_path.string());
  }

  return full_path.string();
}

std::string DownloadManager::DownloadModel(const ModelInfo& info,
                                           std::function<int(float)> progress_cb) {
  // Serialize all model downloads in this process: only one runs at a time, so it
  // gets the full network and disk instead of competing with another download.
  // The cross-process file lock taken below extends the guarantee across every
  // process and app that shares this cache directory.
  std::unique_lock<std::mutex> download_guard(download_mutex_);
  auto model_path = ComputeModelPath(info);

  // Fast path: serve the cache without taking the cross-process lock.
  // A valid cache hit requires: directory exists, no in-progress signal file, and
  // inference_model.json is present (written by DownloadModel on successful completion).
  auto signal_path = std::filesystem::path(model_path) / kDownloadSignalFileName;
  if (std::filesystem::exists(model_path) && !std::filesystem::exists(signal_path) &&
      HasInferenceModelJson(model_path)) {
    // Already cached and download was complete — cancellation request is
    // meaningless at 100%, so the return value is intentionally ignored.
    if (progress_cb) {
      progress_cb(100.0f);
    }

    return ResolveEffectiveModelPath(model_path);
  }

  if (info.uri.empty()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "cannot download model: empty URI (asset_id)");
  }

  // Create output directory before taking the cross-process lock, since the lock
  // file lives inside it.
  std::filesystem::create_directories(model_path);

  // Serialize across processes that share this cache directory. Inside the
  // running process the download mutex already serializes downloads; the file
  // lock protects against a second SDK instance (e.g. another service or CLI)
  // racing on the same model directory.
  auto cancel_pred = [&progress_cb]() -> bool {
    // progress_cb returning non-zero is the SDK's cancellation signal. Reusing
    // it here also acts as a periodic heartbeat (0%) while we wait for the
    // other process to finish.
    return progress_cb && progress_cb(0.0f) != 0;
  };
  auto lock = CrossProcessFileLock::TryAcquireForDirectory(model_path, logger_);
  if (!lock) {
    logger_.Log(LogLevel::Information,
                "Model download is being performed by another process. Waiting on lock at '" +
                    model_path + "'...");
    // Don't hold the in-process download mutex while blocking on the cross-process
    // lock: that wait can last minutes to hours (another process is downloading),
    // and freezing every unrelated in-process model download for that long is far
    // worse than the bandwidth contention this mutex exists to prevent. Release it
    // for the wait and re-acquire before the cache re-check + download below.
    //
    // Re-acquiring after the wait can briefly hold this model's file lock while a
    // different model's download owns the mutex: this model's lock is then held but
    // idle, so another process waiting on it is blocked until the in-process download
    // ahead finishes. That window is bounded — the mutex holder is always making
    // progress and releases it — not a deadlock. Deadlock-freedom relies on the
    // blocking file-lock wait staying outside the mutex: the fast path above locks
    // mutex-then-file-lock while this path locks file-lock-then-mutex, and the
    // opposite order is only safe because the *blocking* file-lock acquire
    // (WaitForDirectoryLock) never runs while the mutex is held — the in-line acquire
    // at the top is the non-blocking TryAcquireForDirectory. Keep it that way.
    download_guard.unlock();
    lock = CrossProcessFileLock::WaitForDirectoryLock(model_path, cancel_pred, logger_);
    download_guard.lock();
  }

  // Another process may have just completed the download we were waiting on.
  // Re-check the cache now that we hold the lock.
  if (std::filesystem::exists(model_path) && !std::filesystem::exists(signal_path) &&
      HasInferenceModelJson(model_path)) {
    if (progress_cb) {
      progress_cb(100.0f);
    }
    return ResolveEffectiveModelPath(model_path);
  }

  // Create download signal file
  {
    std::ofstream signal(signal_path);
    // Empty file — its presence indicates download is in progress
  }

  // Emit 0% immediately so callers know the download process has started.
  // This provides a heartbeat during the silent container resolution phase.
  if (progress_cb && progress_cb(0.0f) != 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "download cancelled by user progress callback");
  }

  try {
    // Step 1: Resolve SAS URI from the region that served this model's catalog entry
    // (or the explicit override / default registry-region fallback).
    auto container = registry_client_->ResolveModelContainer(info.uri, ResolveRegion(config_region_, info));

    if (container.blob_sas_uri.empty()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "model registry returned empty SAS URI for: " + info.uri);
    }

    // Step 2: Download blobs
    BlobDownloadOptions download_opts;
    // The ModelInfo doesn't currently carry a path prefix, but the URI is the asset_id
    // and the blob container has all files at the root or in variant subdirectories.
    download_opts.path_prefix = "";
    download_opts.max_concurrency = max_concurrency_;

    if (progress_cb) {
      download_opts.progress = [&progress_cb](float percent) {
        return progress_cb(percent);
      };
    }

    DownloadBlobsToDirectory(*blob_downloader_, container.blob_sas_uri,
                             model_path, download_opts);

    // Step 3: Write inference_model.json — use model_id (includes version) so the
    // local model scanner can match it back to catalog entries during startup.
    WriteInferenceModelJson(model_path, info.model_id, info.prompt_templates);

    // Step 4: Fix variant download
    FixVariantInferenceModelJson(model_path);

    // Step 5: Remove download signal — marks download as complete
    std::filesystem::remove(signal_path);

    return ResolveEffectiveModelPath(model_path);
  } catch (...) {
    // Leave the signal file in place so the incomplete download is detected
    throw;
  }
}

bool DownloadManager::IsModelCached(const ModelInfo& info) const {
  auto model_path = ComputeModelPath(info);
  if (!std::filesystem::exists(model_path)) {
    return false;
  }

  // A valid cache requires no in-progress signal file AND inference_model.json
  // (written on successful download completion). An empty directory or one
  // without inference_model.json is not a valid cache hit.
  auto signal_path = std::filesystem::path(model_path) / kDownloadSignalFileName;
  return !std::filesystem::exists(signal_path) && HasInferenceModelJson(model_path);
}

std::string DownloadManager::GetModelCachePath(const ModelInfo& info) const {
  auto model_path = ComputeModelPath(info);
  if (std::filesystem::exists(model_path)) {
    return model_path;
  }

  return {};
}

}  // namespace fl
