// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Shared temp directory / path helpers for tests. CTest (gtest_discover_tests)
// launches a separate process per test, so temp names embed the pid plus a
// per-process counter and never collide across concurrent test processes.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <process.h>  // _getpid
#else
#include <unistd.h>  // getpid
#endif

namespace fl::test {

/// Current process id. process.h is used instead of windows.h so callers that use
/// std::min/std::max aren't broken by its macros.
inline long CurrentPid() {
#ifdef _WIN32
  return ::_getpid();
#else
  return static_cast<long>(::getpid());
#endif
}

/// Build a unique path under the system temp directory as `<prefix><pid>_<counter>`. The pid
/// separates concurrent test processes and the per-process atomic counter separates callers
/// within one process, so no two live temp paths collide — no randomness required.
inline std::filesystem::path MakeUniqueTempPath(const std::string& prefix) {
  static std::atomic<uint64_t> counter{0};
  return std::filesystem::temp_directory_path() /
         (prefix + std::to_string(CurrentPid()) + "_" +
          std::to_string(counter.fetch_add(1, std::memory_order_relaxed)));
}

/// RAII temporary filesystem path for test isolation, removed on destruction so a flaky test
/// never leaks into the system temp dir. The name comes from MakeUniqueTempPath, so it is unique
/// both across the separate processes CTest launches per test and across multiple instances within
/// one test. Construct via the factories:
///   - CreateTempDir  creates the directory up front (create_directory must succeed).
///   - CreateTempFile only reserves the path; the caller creates the file.
class TempPath {
 public:
  /// Create and own a fresh temporary directory. A residual collision (e.g. a directory leaked by
  /// an earlier process that reused this pid) advances to the next name and retries rather than
  /// silently sharing an existing directory.
  static TempPath CreateTempDir(const std::string& prefix = "fl_test_") {
    return TempPath(prefix, /*create_dir=*/true);
  }

  /// Reserve and own a fresh temporary file path. The file is not created here — the caller does.
  static TempPath CreateTempFile(const std::string& prefix = "fl_test_") {
    return TempPath(prefix, /*create_dir=*/false);
  }

  ~TempPath() {
    // Destructors must not throw. remove_all is used via its error_code overload, but guard the
    // whole body so an unexpected exception (e.g. bad_alloc) is reported rather than terminating.
    try {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "TempPath: failed to remove '%s': %s\n", path_.string().c_str(), e.what());
    }
  }

  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;

  const std::filesystem::path& path() const { return path_; }
  std::string string() const { return path_.string(); }

 private:
  TempPath(const std::string& prefix, bool create_dir) {
    while (true) {
      auto candidate = MakeUniqueTempPath(prefix);
      if (!create_dir) {
        path_ = std::move(candidate);
        return;
      }
      std::error_code ec;
      if (std::filesystem::create_directory(candidate, ec)) {
        path_ = std::move(candidate);
        return;
      }
      if (ec) {
        throw std::runtime_error("TempPath: failed to create '" + candidate.string() + "': " +
                                 ec.message());
      }
      // candidate already existed — try the next name.
    }
  }

  std::filesystem::path path_;
};

}  // namespace fl::test
