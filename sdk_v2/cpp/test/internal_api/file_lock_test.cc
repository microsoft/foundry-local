// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Tests for the RAII cross-process FileLock in src/util/file_lock.{h,cc}.
//
// Windows-only by request. The contention cases rely on CreateFileW's exclusive
// share mode (dwShareMode == 0), which denies a second open of the same path even
// within the same process — giving deterministic single-process lock and timeout
// behavior. POSIX flock semantics differ, so these tests do not run there.

#include "util/file_lock.h"

#include <gtest/gtest.h>

#ifdef _WIN32

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

class FileLockFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    base_dir_ = fs::temp_directory_path() /
                ("fl_file_lock_test_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(base_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(base_dir_, ec);
  }

  fs::path base_dir_;
};

}  // namespace

// Acquiring a lock creates the lock file; releasing it leaves the file in place
// and allows the path to be locked again.
TEST_F(FileLockFixture, AcquireCreatesLockFileAndReleaseAllowsReacquire) {
  const fs::path lock_path = base_dir_ / "basic.lock";

  {
    fl::FileLock lock(lock_path);
    EXPECT_TRUE(fs::exists(lock_path));
  }

  // The lock file persists after release (it is not deleted on unlock).
  EXPECT_TRUE(fs::exists(lock_path));

  // Re-acquiring the now-free lock must not throw.
  EXPECT_NO_THROW({ fl::FileLock relock(lock_path); });
}

// The constructor creates any missing parent directories for the lock file.
TEST_F(FileLockFixture, CreatesMissingParentDirectories) {
  const fs::path nested = base_dir_ / "a" / "b" / "c" / "nested.lock";
  ASSERT_FALSE(fs::exists(nested.parent_path()));

  fl::FileLock lock(nested);

  EXPECT_TRUE(fs::exists(nested.parent_path()));
  EXPECT_TRUE(fs::exists(nested));
}

// While one lock is held, a second lock on the same path times out and throws,
// and the error message identifies the contended path.
TEST_F(FileLockFixture, SecondLockTimesOutWhileFirstHeld) {
  const fs::path lock_path = base_dir_ / "contended.lock";

  fl::FileLock first(lock_path);

  // A small positive timeout forces at least one retry/sleep cycle before the
  // deadline elapses, exercising the constructor's wait loop.
  try {
    fl::FileLock second(lock_path, /*timeout_ms=*/200);
    FAIL() << "Expected FileLock to throw while the lock is already held";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find(lock_path.string()), std::string::npos);
  }
}

// Once the first lock is released, the same path can be locked again immediately.
TEST_F(FileLockFixture, SecondLockSucceedsAfterFirstReleased) {
  const fs::path lock_path = base_dir_ / "sequential.lock";

  {
    fl::FileLock first(lock_path);
  }  // released here

  // timeout_ms == 0: the lock is free, so acquisition succeeds on the first try.
  EXPECT_NO_THROW({ fl::FileLock second(lock_path, /*timeout_ms=*/0); });
}

#endif  // _WIN32
