// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "platform/dynlib_loader.h"
#include "logger.h"
#include "utils/scoped_environment_variable.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fl::platform {
namespace {

constexpr std::wstring_view kDependency = L"foundry_loader_test_dependency.dll";

class TestLogger : public ILogger {
 public:
  void Log(LogLevel, std::string_view message) override { messages.emplace_back(message); }
  std::vector<std::string> messages;
};

class WindowsDllLoaderTest : public testing::Test {
 protected:
  test::TempPath root = test::TempPath::CreateTempDir("fl_dll_loader_");
  std::filesystem::path provider_dir = root.path() / "provider";
  std::filesystem::path cuda_dir = root.path() / "CUDA 12 bin";
  std::filesystem::path library = provider_dir / "foundry_loader_test_library.dll";
  TestLogger logger;

  void SetUp() override {
    std::filesystem::create_directory(provider_dir);
    std::filesystem::create_directory(cuda_dir);
    std::filesystem::copy_file(FOUNDRY_TEST_LOADER_LIBRARY, library);
    std::filesystem::copy_file(FOUNDRY_TEST_LOADER_DEPENDENCY, cuda_dir / kDependency);
    ASSERT_EQ(::GetModuleHandleW(kDependency.data()), nullptr);
  }

  void TearDown() override { EXPECT_EQ(::GetModuleHandleW(kDependency.data()), nullptr); }

  void ExpectValue(const std::shared_ptr<void>& handle) {
    ASSERT_NE(handle, nullptr);
    const auto value = reinterpret_cast<int (*)()>(
        ::GetProcAddress(static_cast<HMODULE>(handle.get()), "FoundryLoaderValue"));
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value(), 42);
  }
};

TEST_F(WindowsDllLoaderTest, LoadsPathDependencyAndRetainsItUntilLibraryRelease) {
  test::ScopedEnvironmentVariable path("PATH", cuda_dir.string());
  EXPECT_EQ(LoadSharedLibrary(library, logger), nullptr);
  auto handle = LoadSharedLibraryWithPathFallback(library, logger);
  ExpectValue(handle);
  EXPECT_NE(::GetModuleHandleW(kDependency.data()), nullptr);
  handle.reset();
  EXPECT_EQ(::GetModuleHandleW(kDependency.data()), nullptr);
  EXPECT_EQ(LoadSharedLibrary(library, logger), nullptr);
}

TEST_F(WindowsDllLoaderTest, AddedDllDirectoryWorksWithoutPathAndOrtStyleLoadReusesModule) {
  const auto invalid_dir = root.path() / "invalid";
  std::filesystem::create_directory(invalid_dir);
  std::ofstream(invalid_dir / kDependency) << "not a DLL";
  test::ScopedEnvironmentVariable path("PATH", invalid_dir.string());
  const auto cookie = ::AddDllDirectory(cuda_dir.c_str());
  ASSERT_NE(cookie, nullptr);
  auto handle = LoadSharedLibraryWithPathFallback(library, logger);
  EXPECT_TRUE(::RemoveDllDirectory(cookie));
  ExpectValue(handle);

  const auto ort_handle = ::LoadLibraryExW(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  ASSERT_NE(ort_handle, nullptr);
  EXPECT_EQ(ort_handle, handle.get());
  EXPECT_TRUE(::FreeLibrary(ort_handle));
}

TEST_F(WindowsDllLoaderTest, ProviderDirectoryWorksWithoutPath) {
  std::filesystem::copy_file(cuda_dir / kDependency, provider_dir / kDependency);
  test::ScopedEnvironmentVariable path("PATH", "");
  ExpectValue(LoadSharedLibraryWithPathFallback(library, logger));
}

TEST_F(WindowsDllLoaderTest, SupportsUnicodePathEntry) {
  const auto unicode_dir = root.path() / L"CUDA \u4e2d\u6587 \u03b1";
  std::filesystem::create_directory(unicode_dir);
  std::filesystem::copy_file(cuda_dir / kDependency, unicode_dir / kDependency);
  test::ScopedEnvironmentVariable path("PATH", "");
  ASSERT_EQ(_wputenv_s(L"PATH", unicode_dir.c_str()), 0);
  ExpectValue(LoadSharedLibraryWithPathFallback(library, logger));
}

TEST_F(WindowsDllLoaderTest, ReportsInvalidFirstPathCandidateWithoutTryingAnotherDistribution) {
  const auto invalid_dir = root.path() / "invalid";
  std::filesystem::create_directory(invalid_dir);
  std::ofstream(invalid_dir / kDependency) << "not a DLL";
  test::ScopedEnvironmentVariable path("PATH", invalid_dir.string() + ";" + cuda_dir.string());
  EXPECT_EQ(LoadSharedLibraryWithPathFallback(library, logger), nullptr);
  ASSERT_FALSE(logger.messages.empty());
  EXPECT_NE(logger.messages.back().find("failed to load"), std::string::npos);
  EXPECT_NE(logger.messages.back().find("(193)"), std::string::npos);
  EXPECT_EQ(logger.messages.back().find("CUDA 12"), std::string::npos);
}

TEST_F(WindowsDllLoaderTest, ReportsMissingDependencyWithoutLeakingModules) {
  test::ScopedEnvironmentVariable path("PATH", "");
  EXPECT_EQ(LoadSharedLibraryWithPathFallback(library, logger), nullptr);
  EXPECT_EQ(::GetModuleHandleW(kDependency.data()), nullptr);
  ASSERT_EQ(logger.messages.size(), 1u);
  EXPECT_NE(logger.messages.back().find("(126)"), std::string::npos);
  EXPECT_NE(logger.messages.back().find("one of its dependencies"), std::string::npos);
  EXPECT_NE(logger.messages.back().find("CUDA 12"), std::string::npos);
  EXPECT_NE(logger.messages.back().find("PATH or AddDllDirectory"), std::string::npos);
  EXPECT_NE(logger.messages.back().find("CUDA 13"), std::string::npos);

  logger.messages.clear();
  EXPECT_EQ(LoadSharedLibrary(library, logger), nullptr);
  ASSERT_EQ(logger.messages.size(), 1u);
  EXPECT_EQ(logger.messages.back().find("CUDA 12"), std::string::npos);
}

}  // namespace
}  // namespace fl::platform
