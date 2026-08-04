// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/ep_bundle_installer.h"

#include "http/http_download.h"
#include "logger.h"
#include "util/file_lock.h"
#include "util/sha256.h"
#include "util/string_utils.h"
#include "util/zip_extract.h"

#include <fmt/format.h>

#include <chrono>
#include <cctype>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fl {

namespace {

constexpr int kArchiveHashRetries = 1;
constexpr int kRawHashRetries = 0;

std::string GenerateUniqueId() {
  static thread_local std::mt19937_64 rng(
      std::random_device{}() ^
      static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
  return fmt::format("{:016x}", rng());
}

std::optional<std::string> ReadActiveMarker(const std::filesystem::path& active_path) {
  std::ifstream in(active_path);
  if (!in) {
    return std::nullopt;
  }

  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  while (!content.empty() && (content.back() == '\n' || content.back() == '\r' || content.back() == ' ')) {
    content.pop_back();
  }

  return content.empty() ? std::nullopt : std::make_optional(content);
}

bool AtomicReplaceFile(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
#ifdef _WIN32
  if (::MoveFileExW(from.wstring().c_str(), to.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    ec.assign(static_cast<int>(::GetLastError()), std::system_category());
    return false;
  }

  ec.clear();
  return true;
#else
  std::filesystem::rename(from, to, ec);
  return !ec;
#endif
}

bool PublishActiveMarker(const std::filesystem::path& root_dir, const std::string& generation_id,
                         std::string_view ep_display_name, ILogger& logger) {
  const auto active_path = root_dir / "active";
  const auto tmp_path = root_dir / fmt::format("active.{}.tmp", GenerateUniqueId());

  bool write_ok = false;
  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (out) {
      out << generation_id;
      out.flush();
      out.close();
      write_ok = static_cast<bool>(out);  // true only if the write, flush, and close all succeeded
    }
  }

  if (!write_ok) {
    logger.Log(LogLevel::Warning,
               fmt::format("{}: failed to write active marker temp file '{}'", ep_display_name, tmp_path.string()));
    std::error_code ec;
    std::filesystem::remove(tmp_path, ec);
    return false;
  }

  std::error_code ec;
  if (!AtomicReplaceFile(tmp_path, active_path, ec)) {
    logger.Log(LogLevel::Warning, fmt::format("{}: failed to publish active marker '{}': {}", ep_display_name,
                                              active_path.string(), ec.message()));
    std::filesystem::remove(tmp_path, ec);
    return false;
  }

  return true;
}

bool IsSha256(std::string_view value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

bool IsSafeId(std::string_view value) {
  if (value.empty() || value == "." || value == "..") {
    return false;
  }

  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.';
  });
}

bool IsSafeRelativePath(std::string_view value) {
  if (value.empty() || value.find('\\') != std::string_view::npos) {
    return false;
  }

  const std::filesystem::path path(value);
  if (path.is_absolute() || path.has_root_path() || path.lexically_normal().generic_string() != value) {
    return false;
  }

  for (const auto& component : path) {
    if (component == "." || component == "..") {
      return false;
    }
  }

  return true;
}

bool IsHttpsUrl(std::string_view value) {
  constexpr std::string_view prefix = "https://";
  if (!value.starts_with(prefix) || value.find('#') != std::string_view::npos) {
    return false;
  }

  const auto authority_end = value.find('/', prefix.size());
  const auto authority = value.substr(prefix.size(), authority_end - prefix.size());
  return !authority.empty() && authority.find('@') == std::string_view::npos;
}

bool ValidateManifest(const EpBundleManifest& manifest, std::string_view ep_display_name, ILogger& logger) {
  if (!manifest.IsSupported()) {
    logger.Log(LogLevel::Warning,
               fmt::format("{}: bundle is disabled or incomplete", ep_display_name));
    return false;
  }

  if (!IsSafeId(manifest.bundle_id) || !IsSafeRelativePath(manifest.provider_relative_path)) {
    logger.Log(LogLevel::Warning, fmt::format("{}: bundle manifest contains an invalid path", ep_display_name));
    return false;
  }

  std::unordered_set<std::string> artifact_ids;
  std::unordered_set<std::string> file_paths;

  for (const auto& artifact : manifest.artifacts) {
    if (!IsSafeId(artifact.id) || !artifact_ids.insert(artifact.id).second || !IsHttpsUrl(artifact.url)) {
      logger.Log(LogLevel::Warning, fmt::format("{}: artifact metadata is invalid", ep_display_name));
      return false;
    }

    if (artifact.is_archive) {
      if (!IsSha256(artifact.archive_sha256) || artifact.archive_max_bytes == 0 ||
          artifact.extracted_files.empty()) {
        logger.Log(LogLevel::Warning, fmt::format("{}: archive artifact metadata is incomplete", ep_display_name));
        return false;
      }

      for (const auto& file : artifact.extracted_files) {
        if (!IsSafeRelativePath(file.relative_path) || !IsSha256(file.sha256) ||
            !file_paths.insert(file.relative_path).second) {
          logger.Log(LogLevel::Warning, fmt::format("{}: archive file metadata is invalid", ep_display_name));
          return false;
        }
      }
    } else {
      if (!IsSafeRelativePath(artifact.raw_relative_path) || !IsSha256(artifact.raw_sha256) ||
          artifact.raw_max_bytes == 0 || !file_paths.insert(artifact.raw_relative_path).second) {
        logger.Log(LogLevel::Warning, fmt::format("{}: raw artifact metadata is invalid", ep_display_name));
        return false;
      }
    }
  }

  if (file_paths.count(manifest.provider_relative_path) == 0) {
    logger.Log(LogLevel::Warning, fmt::format("{}: provider is not present in the bundle manifest", ep_display_name));
    return false;
  }

  return true;
}

bool CollectRegularFiles(const std::filesystem::path& dir, std::unordered_set<std::string>& files) {
  std::error_code ec;

  if (!std::filesystem::exists(dir, ec)) {
    return true;
  }

  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
    const auto status = entry.symlink_status(ec);
    if (ec) {
      return false;
    }

    if (std::filesystem::is_regular_file(status)) {
      files.insert(std::filesystem::relative(entry.path(), dir, ec).generic_string());
    } else if (!std::filesystem::is_directory(status)) {
      return false;
    }
  }

  return !ec;
}

bool VerifyRegularFile(const std::filesystem::path& path, std::string_view expected_hash) {
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(path, ec);
  return !ec && std::filesystem::is_regular_file(status) &&
         CompareCaseInsensitive(Sha256File(path), std::string(expected_hash)) == 0;
}

bool VerifyArtifactFiles(const std::filesystem::path& bin_dir, const EpBundleArtifact& artifact) {
  if (artifact.is_archive) {
    return std::all_of(artifact.extracted_files.begin(), artifact.extracted_files.end(), [&](const auto& file) {
      return VerifyRegularFile(bin_dir / file.relative_path, file.sha256);
    });
  }

  return VerifyRegularFile(bin_dir / artifact.raw_relative_path, artifact.raw_sha256);
}

bool CopyArtifactFiles(const std::filesystem::path& source_bin, const std::filesystem::path& staging_bin,
                       const EpBundleArtifact& artifact) {
  std::vector<std::string> paths;
  if (artifact.is_archive) {
    for (const auto& file : artifact.extracted_files) {
      paths.push_back(file.relative_path);
    }
  } else {
    paths.push_back(artifact.raw_relative_path);
  }

  std::error_code ec;
  for (const auto& relative_path : paths) {
    const auto source = source_bin / relative_path;
    const auto destination = staging_bin / relative_path;
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec || !std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing,
                                          ec)) {
      return false;
    }
  }

  return VerifyArtifactFiles(staging_bin, artifact);
}

bool VerifyBundleDir(const std::filesystem::path& bin_dir, const EpBundleManifest& manifest,
                     std::string_view ep_display_name, ILogger& logger) {
  std::unordered_set<std::string> expected;

  for (const auto& artifact : manifest.artifacts) {
    if (artifact.is_archive) {
      for (const auto& file : artifact.extracted_files) {
        expected.insert(file.relative_path);
      }
    } else {
      expected.insert(artifact.raw_relative_path);
    }

    if (!VerifyArtifactFiles(bin_dir, artifact)) {
      logger.Log(LogLevel::Warning, fmt::format("{}: bundle '{}' has invalid files for artifact '{}'",
                                                ep_display_name, manifest.bundle_id, artifact.id));
      return false;
    }
  }

  std::unordered_set<std::string> actual;
  if (!CollectRegularFiles(bin_dir, actual)) {
    logger.Log(LogLevel::Warning,
               fmt::format("{}: bundle '{}' contains an unsupported filesystem entry", ep_display_name,
                           manifest.bundle_id));
    return false;
  }

  if (actual.size() != expected.size()) {
    logger.Log(LogLevel::Warning,
               fmt::format("{}: bundle '{}' contains unexpected files ({} present, {} expected)", ep_display_name,
                           manifest.bundle_id, actual.size(), expected.size()));
    return false;
  }

  return true;
}

void CleanupStaleGenerations(const std::filesystem::path& bundles_dir, const std::filesystem::path& staging_root,
                             const std::unordered_set<std::string>& keep_ids, std::string_view ep_display_name,
                             ILogger& logger) {
  std::error_code ec;

  if (std::filesystem::exists(staging_root, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(staging_root, ec)) {
      std::filesystem::remove_all(entry.path(), ec);
    }
  }

  if (!std::filesystem::exists(bundles_dir, ec)) {
    return;
  }

  for (const auto& entry : std::filesystem::directory_iterator(bundles_dir, ec)) {
    if (keep_ids.count(entry.path().filename().string()) == 0) {
      logger.Log(LogLevel::Debug,
                 fmt::format("{}: removing orphaned bundle generation '{}'", ep_display_name,
                             entry.path().filename().string()));
      std::filesystem::remove_all(entry.path(), ec);
    }
  }
}

void HardenFilePermissions(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::permissions(path,
                               std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                   std::filesystem::perms::group_read | std::filesystem::perms::others_read,
                               std::filesystem::perm_options::replace, ec);
}

bool DownloadWithHashRetry(const EpArtifactDownloadFn& download_fn, const std::string& url,
                           const std::filesystem::path& destination, uint64_t max_bytes,
                           const std::string& expected_sha256, int max_retries, std::string_view artifact_id,
                           std::string_view ep_display_name, float base_pct, float span_pct,
                           const IEpBootstrapper::ProgressCallback& progress_cb, ILogger& logger) {
  for (int attempt = 0; attempt <= max_retries; ++attempt) {
    std::error_code ec;
    std::filesystem::remove(destination, ec);

    std::atomic<bool> cancel_flag{false};
    auto local_progress = [&](float pct) {
      if (progress_cb && !progress_cb(std::string(ep_display_name), base_pct + pct * span_pct / 100.0f)) {
        cancel_flag.store(true);
      }
    };

    if (!download_fn(url, destination, max_bytes, &cancel_flag, local_progress, logger)) {
      logger.Log(LogLevel::Warning,
                 fmt::format("{}: download failed for artifact '{}'", ep_display_name, artifact_id));
      return false;
    }

    auto hash = Sha256File(destination);
    if (CompareCaseInsensitive(hash, expected_sha256) == 0) {
      return true;
    }

    logger.Log(LogLevel::Warning,
               fmt::format("{}: hash mismatch for artifact '{}' (attempt {}/{}): got {}, expected {}",
                           ep_display_name, artifact_id, attempt + 1, max_retries + 1, hash, expected_sha256));
  }

  return false;
}

bool InstallArchiveArtifact(const EpArtifactDownloadFn& download_fn, const EpBundleArtifact& artifact,
                            const std::filesystem::path& staging_dir, const std::filesystem::path& staging_bin,
                            float base_pct, float span_pct, std::string_view ep_display_name,
                            const IEpBootstrapper::ProgressCallback& progress_cb, ILogger& logger) {
  auto archive_path = staging_dir / (artifact.id + ".archive");

  if (!DownloadWithHashRetry(download_fn, artifact.url, archive_path, artifact.archive_max_bytes,
                             artifact.archive_sha256, kArchiveHashRetries, artifact.id, ep_display_name, base_pct,
                             span_pct, progress_cb, logger)) {
    return false;
  }

  if (!ExtractZip(archive_path, staging_bin, logger)) {
    logger.Log(LogLevel::Warning, fmt::format("{}: extraction failed for artifact '{}'", ep_display_name, artifact.id));
    return false;
  }

  std::error_code ec;
  std::filesystem::remove(archive_path, ec);

  for (const auto& file : artifact.extracted_files) {
    auto file_path = staging_bin / file.relative_path;
    if (!std::filesystem::is_regular_file(file_path, ec)) {
      logger.Log(LogLevel::Warning,
                 fmt::format("{}: artifact '{}' is missing expected extracted file '{}'", ep_display_name,
                             artifact.id, file.relative_path));
      return false;
    }

    auto hash = Sha256File(file_path);
    if (CompareCaseInsensitive(hash, file.sha256) != 0) {
      logger.Log(LogLevel::Warning,
                 fmt::format("{}: extracted file '{}' from artifact '{}' hash mismatch: got {}, expected {}",
                             ep_display_name, file.relative_path, artifact.id, hash, file.sha256));
      return false;
    }

    HardenFilePermissions(file_path);
  }

  return true;
}

bool InstallRawArtifact(const EpArtifactDownloadFn& download_fn, const EpBundleArtifact& artifact,
                        const std::filesystem::path& staging_bin, float base_pct, float span_pct,
                        std::string_view ep_display_name, const IEpBootstrapper::ProgressCallback& progress_cb,
                        ILogger& logger) {
  auto raw_path = staging_bin / artifact.raw_relative_path;
  std::filesystem::create_directories(raw_path.parent_path());

  if (!DownloadWithHashRetry(download_fn, artifact.url, raw_path, artifact.raw_max_bytes, artifact.raw_sha256,
                             kRawHashRetries, artifact.id, ep_display_name, base_pct, span_pct, progress_cb, logger)) {
    return false;
  }

  HardenFilePermissions(raw_path);
  return true;
}

EpArtifactDownloadFn DefaultDownloadFn() {
  return [](const std::string& url, const std::filesystem::path& destination, uint64_t max_bytes,
            std::atomic<bool>* cancel_flag, const std::function<void(float)>& progress_cb, ILogger& logger) {
    const int64_t cap = max_bytes == 0 ? -1 : static_cast<int64_t>(max_bytes);
    return HttpDownloadFile(url, destination, "FoundryLocal", cancel_flag, progress_cb, logger, cap);
  };
}

}  // namespace

EpBundleInstaller::EpBundleInstaller(std::filesystem::path root_dir, std::string lock_file_name,
                                     std::string ep_display_name, EpArtifactDownloadFn download_fn)
    : root_dir_(std::filesystem::absolute(std::move(root_dir))),
      lock_file_name_(std::move(lock_file_name)),
      ep_display_name_(std::move(ep_display_name)),
      download_fn_(download_fn ? std::move(download_fn) : DefaultDownloadFn()) {}

std::unique_ptr<EpInstallTransaction> EpBundleInstaller::EnsureInstalled(
    const EpBundleManifest& manifest, const IEpBootstrapper::ProgressCallback& progress_cb, ILogger& logger,
    EpBundleInstallPolicy policy) {
  if (!ValidateManifest(manifest, ep_display_name_, logger)) {
    return nullptr;
  }

  try {
    std::filesystem::create_directories(root_dir_);
    auto lock = std::make_unique<FileLock>(root_dir_ / lock_file_name_);

    auto bundles_dir = root_dir_ / "bundles";
    auto staging_root = root_dir_ / "staging";
    auto active_generation = ReadActiveMarker(root_dir_ / "active");

    std::unordered_set<std::string> keep_ids;
    if (active_generation.has_value() && IsSafeId(*active_generation)) {
      keep_ids.insert(*active_generation);
    } else {
      active_generation.reset();
    }
    CleanupStaleGenerations(bundles_dir, staging_root, keep_ids, ep_display_name_, logger);

    std::filesystem::path active_bin;
    if (active_generation.has_value()) {
      active_bin = bundles_dir / *active_generation / "bin";
    }

    if (policy == EpBundleInstallPolicy::ReuseVerified && !active_bin.empty() &&
        VerifyBundleDir(active_bin, manifest, ep_display_name_, logger)) {
      logger.Log(LogLevel::Information,
                 fmt::format("{}: reusing verified bundle '{}'", ep_display_name_, manifest.bundle_id));
      if (progress_cb) {
        progress_cb(ep_display_name_, 90.0f);
      }
      return std::unique_ptr<EpInstallTransaction>(
          new EpInstallTransaction(std::move(lock), root_dir_, ep_display_name_, manifest, *active_generation,
                                   active_bin));
    }

    auto staging_dir = staging_root / GenerateUniqueId();
    auto staging_bin = staging_dir / "bin";
    std::filesystem::create_directories(staging_bin);

    logger.Log(LogLevel::Information,
               fmt::format("{}: installing bundle '{}'", ep_display_name_, manifest.bundle_id));

    const size_t artifact_count = manifest.artifacts.size();
    for (size_t i = 0; i < artifact_count; ++i) {
      const auto& artifact = manifest.artifacts[i];
      const float base_pct = (static_cast<float>(i) / static_cast<float>(artifact_count)) * 80.0f;
      const float span_pct = 80.0f / static_cast<float>(artifact_count);

      bool ok = false;
      if (policy == EpBundleInstallPolicy::ReuseVerified && !active_bin.empty() &&
          VerifyArtifactFiles(active_bin, artifact)) {
        ok = CopyArtifactFiles(active_bin, staging_bin, artifact);
      } else {
        ok = artifact.is_archive
                 ? InstallArchiveArtifact(download_fn_, artifact, staging_dir, staging_bin, base_pct, span_pct,
                                          ep_display_name_, progress_cb, logger)
                 : InstallRawArtifact(download_fn_, artifact, staging_bin, base_pct, span_pct, ep_display_name_,
                                      progress_cb, logger);
      }

      if (!ok) {
        std::error_code ec;
        std::filesystem::remove_all(staging_dir, ec);
        return nullptr;
      }
    }

    if (!VerifyBundleDir(staging_bin, manifest, ep_display_name_, logger)) {
      logger.Log(LogLevel::Warning,
                 fmt::format("{}: staged bundle '{}' failed verification before publish", ep_display_name_,
                             manifest.bundle_id));
      std::error_code ec;
      std::filesystem::remove_all(staging_dir, ec);
      return nullptr;
    }

    std::filesystem::create_directories(bundles_dir);
    const auto generation_id = manifest.bundle_id + "-" + GenerateUniqueId();
    auto final_bundle_dir = bundles_dir / generation_id;
    std::filesystem::rename(staging_dir, final_bundle_dir);

    logger.Log(LogLevel::Information,
               fmt::format("{}: installed bundle '{}'", ep_display_name_, manifest.bundle_id));

    if (progress_cb) {
      progress_cb(ep_display_name_, 90.0f);
    }

    return std::unique_ptr<EpInstallTransaction>(
        new EpInstallTransaction(std::move(lock), root_dir_, ep_display_name_, manifest, generation_id,
                                 final_bundle_dir / "bin"));
  } catch (const std::exception& e) {
    logger.Log(LogLevel::Warning, fmt::format("{}: install error: {}", ep_display_name_, e.what()));
    return nullptr;
  }
}

EpInstallTransaction::EpInstallTransaction(std::unique_ptr<FileLock> lock, std::filesystem::path root_dir,
                                           std::string ep_display_name, EpBundleManifest manifest,
                                           std::string generation_id, std::filesystem::path bin_dir)
    : lock_(std::move(lock)),
      root_dir_(std::move(root_dir)),
      ep_display_name_(std::move(ep_display_name)),
      manifest_(std::move(manifest)),
      generation_id_(std::move(generation_id)),
      bin_dir_(std::move(bin_dir)) {}

EpInstallTransaction::~EpInstallTransaction() = default;

bool EpInstallTransaction::CommitActive(ILogger& logger) {
  if (committed_) {
    return true;
  }

  try {
    if (!VerifyBundleDir(bin_dir_, manifest_, ep_display_name_, logger)) {
      logger.Log(LogLevel::Warning,
                 fmt::format("{}: bundle '{}' failed re-verification before activation; active marker unchanged",
                             ep_display_name_, manifest_.bundle_id));
      return false;
    }

    if (!PublishActiveMarker(root_dir_, generation_id_, ep_display_name_, logger)) {
      return false;
    }

    committed_ = true;
    CleanupStaleGenerations(root_dir_ / "bundles", root_dir_ / "staging", {generation_id_}, ep_display_name_,
                            logger);
    return true;
  } catch (const std::exception& e) {
    logger.Log(LogLevel::Warning, fmt::format("{}: failed to commit active bundle: {}", ep_display_name_, e.what()));
    return false;
  }
}

}  // namespace fl
