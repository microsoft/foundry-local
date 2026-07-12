// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "download/cross_process_file_lock.h"
#include "test_helpers.h"

#include "exception.h"

#include <foundry_local/foundry_local_c.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

using namespace fl;

namespace {

/// Per-test temp directory. Auto-cleans on destruction so a flaky test never
/// leaks lock files into the system temp dir.
class TempDir {
 public:
  TempDir() {
    path_ = fl::test::MakeUniqueTempPath("fl_lock_test_");
    fs::create_directories(path_);
  }

  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

}  // namespace

TEST(CrossProcessFileLockTest, TryAcquireSucceedsForFreshDirectory) {
  TempDir dir;

  auto lock = CrossProcessFileLock::TryAcquireForDirectory(dir.path(), fl::test::NullLog());

  ASSERT_NE(lock, nullptr);
  EXPECT_TRUE(fs::exists(lock->path()));
  EXPECT_EQ(lock->path().parent_path(), dir.path());
  EXPECT_EQ(lock->path().filename(), ".download.lock");
}

TEST(CrossProcessFileLockTest, ReleaseOnDestructionRemovesLockFile) {
  TempDir dir;
  fs::path lock_file;

  {
    auto lock = CrossProcessFileLock::TryAcquireForDirectory(dir.path(), fl::test::NullLog());
    ASSERT_NE(lock, nullptr);
    lock_file = lock->path();
    EXPECT_TRUE(fs::exists(lock_file));
  }

  // After RAII release the lock file should be gone (Win FILE_FLAG_DELETE_ON_CLOSE,
  // POSIX explicit unlink in destructor).
  EXPECT_FALSE(fs::exists(lock_file));
}

TEST(CrossProcessFileLockTest, SecondAcquireReturnsNullWhileFirstIsHeld) {
  TempDir dir;
  auto first = CrossProcessFileLock::TryAcquireForDirectory(dir.path(), fl::test::NullLog());
  ASSERT_NE(first, nullptr);

  auto second = CrossProcessFileLock::TryAcquireForDirectory(dir.path(), fl::test::NullLog());
  EXPECT_EQ(second, nullptr);
}

TEST(CrossProcessFileLockTest, ReacquireSucceedsAfterRelease) {
  TempDir dir;
  {
    auto first = CrossProcessFileLock::TryAcquireForDirectory(dir.path(), fl::test::NullLog());
    ASSERT_NE(first, nullptr);
  }
  auto reacquired = CrossProcessFileLock::TryAcquireForDirectory(dir.path(), fl::test::NullLog());
  EXPECT_NE(reacquired, nullptr);
}

TEST(CrossProcessFileLockTest, CreatesDirectoryIfMissing) {
  TempDir parent;
  auto missing = parent.path() / "nested" / "model";

  ASSERT_FALSE(fs::exists(missing));

  auto lock = CrossProcessFileLock::TryAcquireForDirectory(missing, fl::test::NullLog());

  ASSERT_NE(lock, nullptr);
  EXPECT_TRUE(fs::is_directory(missing));
  EXPECT_TRUE(fs::exists(missing / ".download.lock"));
}

TEST(CrossProcessFileLockTest, WaitForLockReturnsImmediatelyWhenAvailable) {
  TempDir dir;

  auto start = std::chrono::steady_clock::now();
  auto lock = CrossProcessFileLock::WaitForDirectoryLock(dir.path(), []() { return false; }, fl::test::NullLog());
  auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_NE(lock, nullptr);
  // Fast-path acquisition should be well under 100 ms.
  EXPECT_LT(elapsed, std::chrono::milliseconds(500));
}

TEST(CrossProcessFileLockTest, WaitForLockAcquiresAfterHolderReleases) {
  TempDir dir;
  auto holder = CrossProcessFileLock::TryAcquireForDirectory(dir.path(), fl::test::NullLog());
  ASSERT_NE(holder, nullptr);

  // Release the holder after a short delay on another thread.
  std::thread releaser([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    holder.reset();
  });

  auto start = std::chrono::steady_clock::now();
  auto lock = CrossProcessFileLock::WaitForDirectoryLock(
      dir.path(), []() { return false; }, /*logger=*/fl::test::NullLog(),
      /*poll_interval=*/std::chrono::milliseconds(100), /*timeout=*/std::chrono::seconds(10));
  auto elapsed = std::chrono::steady_clock::now() - start;

  releaser.join();
  ASSERT_NE(lock, nullptr);
  EXPECT_GE(elapsed, std::chrono::milliseconds(200));
  EXPECT_LT(elapsed, std::chrono::seconds(5));
}

TEST(CrossProcessFileLockTest, WaitForLockThrowsOnCancellation) {
  TempDir dir;
  auto holder = CrossProcessFileLock::TryAcquireForDirectory(dir.path(), fl::test::NullLog());
  ASSERT_NE(holder, nullptr);

  std::atomic<bool> cancel{false};
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    cancel.store(true);
  });

  try {
    (void)CrossProcessFileLock::WaitForDirectoryLock(
        dir.path(), [&cancel]() { return cancel.load(); }, /*logger=*/fl::test::NullLog(),
        /*poll_interval=*/std::chrono::milliseconds(100), /*timeout=*/std::chrono::seconds(10));
    canceller.join();
    FAIL() << "expected fl::Exception(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED)";
  } catch (const Exception& ex) {
    canceller.join();
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED);
  }
}

TEST(CrossProcessFileLockTest, WaitForLockThrowsOnTimeout) {
  TempDir dir;
  auto holder = CrossProcessFileLock::TryAcquireForDirectory(dir.path(), fl::test::NullLog());
  ASSERT_NE(holder, nullptr);

  try {
    (void)CrossProcessFileLock::WaitForDirectoryLock(
        dir.path(), []() { return false; }, /*logger=*/fl::test::NullLog(),
        /*poll_interval=*/std::chrono::milliseconds(50), /*timeout=*/std::chrono::milliseconds(200));
    FAIL() << "expected fl::Exception(FOUNDRY_LOCAL_ERROR_INTERNAL)";
  } catch (const Exception& ex) {
    EXPECT_EQ(ex.code(), FOUNDRY_LOCAL_ERROR_INTERNAL);
    std::string what = ex.what();
    EXPECT_NE(what.find("timed out"), std::string::npos);
  }
}

#ifndef _WIN32
// A genuine cross-PROCESS test (POSIX, i.e. macOS/Linux): fork a child that
// holds the lock, then verify (a) this process is locked out while the child
// holds it and (b) the kernel releases the flock when the child *exits* — even
// though the child leaves the lock file on disk, mirroring a downloader that
// crashed mid-download. Windows share-none contention is already covered
// in-process by SecondAcquireReturnsNullWhileFirstIsHeld (dwShareMode=0 is
// enforced identically for same- and cross-process opens).
TEST(CrossProcessFileLockTest, HeldAcrossProcessesAndReleasedWhenHolderExits) {
  TempDir dir;
  const auto acquired_signal = dir.path() / "child_acquired";
  const auto release_signal = dir.path() / "parent_done";

  const pid_t pid = ::fork();
  ASSERT_NE(pid, -1) << "fork failed";

  if (pid == 0) {
    // CHILD: acquire, announce, wait (bounded) for the parent, then _exit while
    // still holding it. _exit skips C++/gtest teardown — correct for a forked
    // child — so the lock's destructor never runs and the file is left behind;
    // the kernel still drops the flock on process exit.
    auto lock = CrossProcessFileLock::TryAcquireForDirectory(dir.path(), fl::test::NullLog());
    if (lock == nullptr) {
      _exit(2);
    }
    { std::ofstream(acquired_signal).put('x'); }
    for (int i = 0; i < 200 && !fs::exists(release_signal); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    _exit(0);
  }

  // PARENT: wait for the child to take the lock (up to ~5 s).
  bool child_acquired = false;
  for (int i = 0; i < 200 && !child_acquired; ++i) {
    if (fs::exists(acquired_signal)) {
      child_acquired = true;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }
  ASSERT_TRUE(child_acquired) << "child process never acquired the lock";

  // A different process holds it — we must be locked out.
  EXPECT_EQ(CrossProcessFileLock::TryAcquireForDirectory(dir.path(), fl::test::NullLog()), nullptr);

  // Release the child and reap it.
  { std::ofstream(release_signal).put('x'); }
  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  EXPECT_TRUE(WIFEXITED(status)) << "child did not exit normally";
  EXPECT_EQ(WEXITSTATUS(status), 0) << "child failed to acquire the lock";

  // The holder process is gone: the kernel released its flock even though the
  // lock file is still on disk, so the next acquirer simply re-locks it.
  auto reacquired = CrossProcessFileLock::TryAcquireForDirectory(dir.path(), fl::test::NullLog());
  EXPECT_NE(reacquired, nullptr) << "lock not released after the holder process exited";
}
#endif  // !_WIN32
