// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Focused unit tests for CudaEpBootstrapper's FOUNDRY_LOCAL_CUDA_EP_LIBRARY override path,
// specifically the GenAI runtime DLL preload added to fix Win32 error 126 at model-load time
// (root-caused in cuda_ep_bootstrapper.cc: the DLL was present and hash-verified but GenAI's own
// module-name lookup still failed to find it). These tests never touch the network — the
// override branch resolves purely from local files, so DownloadAndRegister never reaches the
// download/extract code path.
#include "ep_detection/cuda_ep_bootstrapper.h"

#include "internal_api/test_helpers.h"
#include "logger.h"
#include "utils.h"

#include <gtest/gtest.h>

#ifdef _WIN32

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace fl;

namespace {

constexpr const char* kOverrideEnv = "FOUNDRY_LOCAL_CUDA_EP_LIBRARY";
constexpr const char* kProviderDllName = "onnxruntime_providers_cuda.dll";
constexpr const char* kGenaiDllName = "onnxruntime-genai-cuda.dll";

/// Saves/restores FOUNDRY_LOCAL_CUDA_EP_LIBRARY around a test so tests don't leak global env
/// state into other tests running in the same process (gtest_discover_tests launches one process
/// per test case by default, but keeping this scoped costs nothing and removes that assumption).
class ScopedOverrideEnv {
 public:
  explicit ScopedOverrideEnv(const std::filesystem::path& value) {
    auto existing = Utils::GetEnv(kOverrideEnv);
    if (existing.has_value()) {
      previous_ = *existing;
    }
    _putenv_s(kOverrideEnv, value.string().c_str());
  }

  ~ScopedOverrideEnv() { _putenv_s(kOverrideEnv, previous_.value_or("").c_str()); }

  ScopedOverrideEnv(const ScopedOverrideEnv&) = delete;
  ScopedOverrideEnv& operator=(const ScopedOverrideEnv&) = delete;

 private:
  std::optional<std::string> previous_;
};

/// Copies a known-loadable system DLL to `dest` so PreloadAbsoluteDll has a real PE image.
void WriteValidDll(const std::filesystem::path& dest) {
  wchar_t system_dir[MAX_PATH];
  ASSERT_NE(::GetSystemDirectoryW(system_dir, MAX_PATH), 0u);
  std::filesystem::copy_file(std::filesystem::path(system_dir) / L"kernel32.dll", dest,
                             std::filesystem::copy_options::overwrite_existing);
}

/// Writes a file that exists but is not a valid PE image, to exercise the preload-failure path.
void WriteBogusFile(const std::filesystem::path& dest) {
  std::ofstream out(dest, std::ios::binary);
  out << "not a real PE image";
}

}  // namespace

class CudaEpBootstrapperOverrideTest : public ::testing::Test {
 protected:
  std::filesystem::path dir_;
  fl::test::NullLogger logger_;

  void SetUp() override {
    dir_ = fl::test::MakeUniqueTempPath("foundry-cuda-ep-override-test-");
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override {
    // Tests that preload a real DLL (e.g. SiblingGenaiDllValid_RegistersSuccessfully) keep its
    // backing file locked by design — PreloadAbsoluteDll deliberately never calls FreeLibrary,
    // since the module must stay resident for the life of the process. That can make removal of
    // this directory fail with ERROR_SHARING_VIOLATION; that's expected, not a test failure, so
    // use the non-throwing overload and ignore the result.
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
};

TEST_F(CudaEpBootstrapperOverrideTest, SiblingGenaiDllMissing_FailsBeforeRegistration) {
  // Provider DLL "exists" (existence is all the bootstrapper checks for it), but its sibling
  // GenAI runtime DLL is absent — the override should fail up front instead of registering an EP
  // that will blow up later at model-load time.
  auto provider_path = dir_ / kProviderDllName;
  WriteValidDll(provider_path);

  bool register_called = false;
  CudaEpBootstrapper bootstrapper(
      (dir_ / "unused-ep-dir").string(),
      [&](const std::string&, const std::filesystem::path&) {
        register_called = true;
        return true;
      });

  ScopedOverrideEnv scoped_env(provider_path);
  EXPECT_FALSE(bootstrapper.DownloadAndRegister(/*force=*/false, nullptr, logger_));
  EXPECT_FALSE(register_called);
  EXPECT_FALSE(bootstrapper.IsRegistered());
}

TEST_F(CudaEpBootstrapperOverrideTest, SiblingGenaiDllInvalidPeImage_FailsBeforeRegistration) {
  // Sibling file exists at the expected name but isn't a loadable PE image — PreloadAbsoluteDll
  // must fail, and that failure must block registration rather than being silently ignored.
  auto provider_path = dir_ / kProviderDllName;
  WriteValidDll(provider_path);
  WriteBogusFile(dir_ / kGenaiDllName);

  bool register_called = false;
  CudaEpBootstrapper bootstrapper(
      (dir_ / "unused-ep-dir").string(),
      [&](const std::string&, const std::filesystem::path&) {
        register_called = true;
        return true;
      });

  ScopedOverrideEnv scoped_env(provider_path);
  EXPECT_FALSE(bootstrapper.DownloadAndRegister(/*force=*/false, nullptr, logger_));
  EXPECT_FALSE(register_called);
  EXPECT_FALSE(bootstrapper.IsRegistered());
}

TEST_F(CudaEpBootstrapperOverrideTest, SiblingGenaiDllValid_RegistersSuccessfully) {
  // Both the provider DLL and its GenAI sibling are present and loadable — the override should
  // preload the sibling and proceed to register exactly as before.
  auto provider_path = dir_ / kProviderDllName;
  WriteValidDll(provider_path);
  WriteValidDll(dir_ / kGenaiDllName);

  bool register_called = false;
  std::filesystem::path registered_path;
  CudaEpBootstrapper bootstrapper(
      (dir_ / "unused-ep-dir").string(),
      [&](const std::string& reg_name, const std::filesystem::path& lib_path) {
        register_called = true;
        registered_path = lib_path;
        EXPECT_EQ(reg_name, "Foundry.CUDA");
        return true;
      });

  ScopedOverrideEnv scoped_env(provider_path);
  EXPECT_TRUE(bootstrapper.DownloadAndRegister(/*force=*/false, nullptr, logger_));
  EXPECT_TRUE(register_called);
  EXPECT_EQ(registered_path, provider_path);
  EXPECT_TRUE(bootstrapper.IsRegistered());
}

TEST_F(CudaEpBootstrapperOverrideTest, ProviderDllMissing_FailsWithoutTouchingGenaiCheck) {
  // Pre-existing behavior: if the override path itself doesn't exist, fail immediately. This
  // guards against a regression where the new sibling-DLL check would run before this one.
  auto provider_path = dir_ / kProviderDllName;  // never written

  bool register_called = false;
  CudaEpBootstrapper bootstrapper(
      (dir_ / "unused-ep-dir").string(),
      [&](const std::string&, const std::filesystem::path&) {
        register_called = true;
        return true;
      });

  ScopedOverrideEnv scoped_env(provider_path);
  EXPECT_FALSE(bootstrapper.DownloadAndRegister(/*force=*/false, nullptr, logger_));
  EXPECT_FALSE(register_called);
}

#endif  // _WIN32