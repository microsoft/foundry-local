// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Unit tests for ep_detection/ep_utils.h — package verification and the Windows
// DLL-preload helper used by CudaEpBootstrapper to work around GenAI's own
// module-name lookup at model-load time (see cuda_ep_bootstrapper.cc).
#include "ep_detection/ep_utils.h"

#include "logger.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace fl;

namespace {

/// Captures log output so tests can assert the helper logged the path and error details.
class RecordingLogger : public ILogger {
 public:
  std::vector<std::pair<LogLevel, std::string>> entries;

  void Log(LogLevel level, std::string_view message) override {
    entries.emplace_back(level, std::string(message));
  }
};

}  // namespace

// ========================================================================
// PreloadAbsoluteDll
// ========================================================================

#ifdef _WIN32

TEST(PreloadAbsoluteDllTest, ExistingSystemDll_ReturnsTrue) {
  RecordingLogger logger;

  wchar_t system_dir[MAX_PATH];
  ASSERT_NE(::GetSystemDirectoryW(system_dir, MAX_PATH), 0u);
  auto dll_path = std::filesystem::path(system_dir) / L"kernel32.dll";

  EXPECT_TRUE(PreloadAbsoluteDll(dll_path, logger));
  EXPECT_TRUE(logger.entries.empty()) << "should not log anything on success";
}

TEST(PreloadAbsoluteDllTest, CalledTwiceForSamePath_BothCallsSucceed) {
  RecordingLogger logger;

  wchar_t system_dir[MAX_PATH];
  ASSERT_NE(::GetSystemDirectoryW(system_dir, MAX_PATH), 0u);
  auto dll_path = std::filesystem::path(system_dir) / L"kernel32.dll";

  // Preloading the same already-resident module twice must not fail or double-log.
  EXPECT_TRUE(PreloadAbsoluteDll(dll_path, logger));
  EXPECT_TRUE(PreloadAbsoluteDll(dll_path, logger));
  EXPECT_TRUE(logger.entries.empty());
}

TEST(PreloadAbsoluteDllTest, NonexistentPath_ReturnsFalseAndLogsPathAndError) {
  RecordingLogger logger;

  auto dll_path = std::filesystem::temp_directory_path() / "definitely-does-not-exist-foundry.dll";
  ASSERT_FALSE(std::filesystem::exists(dll_path));

  EXPECT_FALSE(PreloadAbsoluteDll(dll_path, logger));

  ASSERT_EQ(logger.entries.size(), 1u);
  EXPECT_EQ(logger.entries[0].first, LogLevel::Warning);
  EXPECT_NE(logger.entries[0].second.find(dll_path.string()), std::string::npos)
      << "log message should include the failing path: " << logger.entries[0].second;
  EXPECT_NE(logger.entries[0].second.find("GetLastError"), std::string::npos)
      << "log message should include the Win32 error code: " << logger.entries[0].second;
}

TEST(PreloadAbsoluteDllTest, ExistingButNotAValidDll_ReturnsFalse) {
  RecordingLogger logger;

  // A file that exists but isn't a valid PE image (e.g. an accidental text file at the expected
  // DLL path) should fail to load rather than crash, and be reported the same way as a missing file.
  auto bogus_path = std::filesystem::temp_directory_path() / "foundry-preload-test-bogus.dll";
  {
    std::ofstream out(bogus_path, std::ios::binary);
    out << "not a real PE image";
  }

  EXPECT_FALSE(PreloadAbsoluteDll(bogus_path, logger));
  ASSERT_EQ(logger.entries.size(), 1u);
  EXPECT_NE(logger.entries[0].second.find(bogus_path.string()), std::string::npos);

  std::filesystem::remove(bogus_path);
}

#else

TEST(PreloadAbsoluteDllTest, NonWindows_AlwaysReturnsFalse) {
  RecordingLogger logger;
  EXPECT_FALSE(PreloadAbsoluteDll("/nonexistent/path.so", logger));
}

#endif  // _WIN32