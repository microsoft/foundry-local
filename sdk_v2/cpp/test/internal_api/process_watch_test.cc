// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/process_watch.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#ifndef FOUNDRY_PROCESS_FIXTURE_PATH
#error Define FOUNDRY_PROCESS_FIXTURE_PATH to the hosting_process_fixture executable path.
#endif

namespace fl::hosting {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

static_assert(!std::is_copy_constructible_v<ProcessWatch>);
static_assert(!std::is_copy_assignable_v<ProcessWatch>);
static_assert(std::is_nothrow_move_constructible_v<ProcessWatch>);
static_assert(std::is_nothrow_move_assignable_v<ProcessWatch>);

#ifdef _WIN32
using Native = HANDLE;
constexpr Native kInvalid = nullptr;

void CloseNative(Native value) {
  if (value != kInvalid) {
    CloseHandle(value);
  }
}

[[noreturn]] void TestFail(const char* operation) {
  throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}
#else
using Native = int;
constexpr Native kInvalid = -1;

void CloseNative(Native value) {
  if (value != kInvalid) {
    close(value);
  }
}

[[noreturn]] void TestFail(const char* operation) {
  throw std::system_error(errno, std::generic_category(), operation);
}
#endif

class OwnedNative {
 public:
  explicit OwnedNative(Native value = kInvalid) noexcept : value_(value) {}
  ~OwnedNative() { CloseNative(value_); }
  OwnedNative(const OwnedNative&) = delete;
  OwnedNative& operator=(const OwnedNative&) = delete;
  OwnedNative(OwnedNative&& other) noexcept : value_(other.Release()) {}
  OwnedNative& operator=(OwnedNative&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }

    return *this;
  }
  [[nodiscard]] Native Get() const noexcept { return value_; }
  [[nodiscard]] Native Release() noexcept { return std::exchange(value_, kInvalid); }
  void Reset(Native value = kInvalid) { CloseNative(std::exchange(value_, value)); }

 private:
  Native value_;
};

struct TestPipe {
  OwnedNative reader;
  OwnedNative writer;
};

TestPipe NewPipe() {
#ifdef _WIN32
  HANDLE reader{}, writer{};
  SECURITY_ATTRIBUTES attributes{
      .nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
  if (!CreatePipe(&reader, &writer, &attributes, 0)) {
    TestFail("CreatePipe");
  }

  return {OwnedNative(reader), OwnedNative(writer)};
#else
  std::array<int, 2> fds{};
#ifdef __linux__
  if (pipe2(fds.data(), O_CLOEXEC) != 0) {
    TestFail("pipe2");
  }
#else
  if (pipe(fds.data()) != 0) {
    TestFail("pipe");
  }
#endif

  TestPipe result{OwnedNative(fds[0]), OwnedNative(fds[1])};
#ifndef __linux__
  if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0) {
    TestFail("fcntl(test pipe)");
  }
#endif
  return result;
#endif
}

// Only this normally-exiting helper is launched. Closing stdin also releases it on test assertion/exception paths.
class Child {
 public:
  Child() { Launch(); }
  ~Child() {
    input_.Reset();
    Reap();
  }
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;

  [[nodiscard]] ProcessIdentity Identity() const {
    std::string line;
    for (char value{}; line.size() < 128;) {
#ifdef _WIN32
      DWORD count{};
      if (!ReadFile(output_.Get(), &value, 1, &count, nullptr) || count != 1) {
        throw std::runtime_error("Process fixture did not report its identity");
      }
#else
      const auto count = read(output_.Get(), &value, 1);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count != 1) {
        throw std::runtime_error("Process fixture did not report its identity");
      }
#endif
      if (value == '\n') {
        ProcessIdentity identity;
        std::istringstream input(line);
        if (!(input >> identity.pid >> identity.generation)) {
          throw std::runtime_error("Process fixture reported a malformed identity");
        }

        return identity;
      }

      line += value;
    }

    throw std::runtime_error("Process fixture identity exceeds its bound");
  }

  void RequestExit() { input_.Reset(); }

#ifndef _WIN32
  void RequestExitLeavingDescendantHoldingOutput() {
    const char command = 'f';
    if (write(input_.Get(), &command, 1) != 1) {
      TestFail("write(fork command)");
    }

    char ready{};
    if (read(output_.Get(), &ready, 1) != 1 || ready != 'h') {
      throw std::runtime_error("Process fixture descendant did not become ready");
    }
  }

  [[nodiscard]] bool OutputHasEof(int timeout) const {
    pollfd event{.fd = output_.Get(), .events = POLLIN, .revents = 0};
    const auto result = poll(&event, 1, timeout);
    if (result < 0) {
      TestFail("poll(fixture output)");
    }

    return result > 0 && (event.revents & POLLHUP) != 0;
  }
#endif

  int Reap() {
#ifdef _WIN32
    if (process_.Get() == kInvalid) {
      return exit_code_;
    }
    if (WaitForSingleObject(process_.Get(), 10000) != WAIT_OBJECT_0) {
      ADD_FAILURE() << "Normally-exiting fixture did not exit within 10 seconds";
      return -1;
    }

    DWORD code{};
    if (!GetExitCodeProcess(process_.Get(), &code)) {
      ADD_FAILURE() << "GetExitCodeProcess failed";
      return -1;
    }

    exit_code_ = static_cast<int>(code);
    process_.Reset();
#else
    if (pid_ <= 0) {
      return exit_code_;
    }

    int status{};
    pid_t result;
    do {
      result = waitpid(pid_, &status, 0);
    } while (result < 0 && errno == EINTR);

    exit_code_ = result == pid_ && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    pid_ = -1;
#endif
    return exit_code_;
  }

 private:
  void Launch() {
    auto input = NewPipe();
    auto output = NewPipe();
#ifdef _WIN32
    if (!SetHandleInformation(input.writer.Get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(output.reader.Get(), HANDLE_FLAG_INHERIT, 0)) {
      TestFail("SetHandleInformation");
    }

    const auto path = std::filesystem::path(FOUNDRY_PROCESS_FIXTURE_PATH).wstring();
    auto command = L"\"" + path + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.reader.Get();
    startup.hStdOutput = output.writer.Get();
    startup.hStdError = output.writer.Get();
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup,
                        &process)) {
      TestFail("CreateProcessW(process fixture)");
    }

    process_.Reset(process.hProcess);
    CloseHandle(process.hThread);
#else
    const auto path = std::filesystem::path(FOUNDRY_PROCESS_FIXTURE_PATH).string();
    std::array<char*, 2> arguments{const_cast<char*>(path.c_str()), nullptr};
    posix_spawn_file_actions_t actions;
    auto error = posix_spawn_file_actions_init(&actions);
    if (error != 0) {
      throw std::system_error(error, std::generic_category(), "posix_spawn_file_actions_init");
    }

    error = posix_spawn_file_actions_adddup2(&actions, input.reader.Get(), STDIN_FILENO);
    if (error == 0) {
      error = posix_spawn_file_actions_adddup2(&actions, output.writer.Get(), STDOUT_FILENO);
    }
    if (error == 0) {
      error = posix_spawn(&pid_, path.c_str(), &actions, nullptr, arguments.data(), environ);
    }

    posix_spawn_file_actions_destroy(&actions);
    if (error != 0) {
      throw std::system_error(error, std::generic_category(), "posix_spawn(process fixture)");
    }
#endif
    input_.Reset(input.writer.Release());
    output_.Reset(output.reader.Release());
  }

  OwnedNative input_;
  OwnedNative output_;
#ifdef _WIN32
  OwnedNative process_;
#else
  pid_t pid_{-1};
#endif
  int exit_code_{-1};
};

TEST(ProcessWatchTest, CurrentIdentityIsStableAndCurrentProcessRemainsAlive) {
  const auto identity = CurrentProcessIdentity();
#ifdef _WIN32
  EXPECT_EQ(identity.pid, GetCurrentProcessId());
#else
  EXPECT_EQ(identity.pid, static_cast<std::uint64_t>(getpid()));
#endif
  EXPECT_NE(identity.generation, 0u);
  EXPECT_EQ(identity, CurrentProcessIdentity());
  ProcessWatch watch(identity);
  EXPECT_EQ(watch.Identity(), identity);
  EXPECT_FALSE(watch.IsExited());
  EXPECT_FALSE(watch.WaitUntil(Clock::now() + 10ms));
  EXPECT_FALSE(watch.IsExited());
}

TEST(ProcessWatchTest, PastDeadlineNeverImpliesCurrentProcessExit) {
  ProcessWatch watch(CurrentProcessIdentity());
  EXPECT_FALSE(watch.WaitUntil(ProcessWatch::TimePoint::min()));
  EXPECT_FALSE(watch.IsExited());
}

TEST(ProcessWatchTest, InvalidIdentitiesAreRejected) {
  const auto current = CurrentProcessIdentity();
  EXPECT_THROW((ProcessWatch(ProcessIdentity{0, current.generation})), std::invalid_argument);
  EXPECT_THROW((ProcessWatch(ProcessIdentity{current.pid, 0})), std::invalid_argument);
  EXPECT_THROW((ProcessWatch(ProcessIdentity{std::numeric_limits<std::uint64_t>::max(), current.generation})),
               std::invalid_argument);
}

TEST(ProcessWatchTest, WrongGenerationIsStaleNotSuccessfulObservation) {
  auto identity = CurrentProcessIdentity();
  identity.generation ^= 1;
  if (identity.generation == 0) {
    identity.generation = 2;
  }

  try {
    ProcessWatch watch(identity);
    FAIL() << "Accepted a mismatched process generation";
  } catch (const ProcessWatchError& error) {
    EXPECT_EQ(error.Failure(), ProcessWatchFailure::kStaleIdentity);
    EXPECT_EQ(error.code(), std::make_error_code(std::errc::no_such_process));
  }
}

TEST(ProcessWatchTest, MoveTransfersObservationAndMovedFromWatchRejectsUse) {
  const auto identity = CurrentProcessIdentity();
  ProcessWatch original(identity);
  ProcessWatch moved(std::move(original));
  EXPECT_EQ(moved.Identity(), identity);
  EXPECT_FALSE(moved.IsExited());
  EXPECT_THROW((void)original.IsExited(), std::logic_error);
  ProcessWatch assigned(identity);
  assigned = std::move(moved);
  EXPECT_FALSE(assigned.IsExited());
  EXPECT_THROW((void)moved.IsExited(), std::logic_error);
}

TEST(ProcessWatchTest, ChildDeadlineIsNotExitAndNormalOsExitRemainsObservedAfterReaping) {
  Child child;
  const auto identity = child.Identity();
  ProcessWatch watch(identity);
  EXPECT_FALSE(watch.IsExited());
  EXPECT_FALSE(watch.WaitUntil(Clock::now() + 10ms));
  EXPECT_FALSE(watch.IsExited());
  child.RequestExit();
  ASSERT_TRUE(watch.WaitUntil(Clock::now() + 10s));
  EXPECT_EQ(child.Reap(), 0);
  EXPECT_EQ(watch.Identity(), identity);
  EXPECT_TRUE(watch.IsExited());
  EXPECT_TRUE(watch.WaitUntil(ProcessWatch::TimePoint::min()));
}

TEST(ProcessWatchTest, OpeningAfterChildReapingNeverAcceptsAnUnverifiedLiveProcess) {
  Child child;
  const auto identity = child.Identity();
  child.RequestExit();
  ASSERT_EQ(child.Reap(), 0);
  try {
    ProcessWatch watch(identity);
    EXPECT_TRUE(watch.IsExited());
  } catch (const ProcessWatchError& error) {
    EXPECT_EQ(error.Failure(), ProcessWatchFailure::kStaleIdentity);
  }
}

TEST(ProcessWatchTest, ChildExitDuringAcquisitionIsObservedOrReportedAsStale) {
  for (int attempt = 0; attempt < 8; ++attempt) {
    Child child;
    const auto identity = child.Identity();
    child.RequestExit();
    try {
      ProcessWatch watch(identity);
      EXPECT_TRUE(watch.WaitUntil(Clock::now() + 10s));
    } catch (const ProcessWatchError& error) {
      EXPECT_EQ(error.Failure(), ProcessWatchFailure::kStaleIdentity);
    }

    EXPECT_EQ(child.Reap(), 0);
  }
}

#ifndef _WIN32
TEST(ProcessWatchTest, ParentOsExitIsObservedWhileDescendantKeepsInheritedOutputOpen) {
  Child child;
  ProcessWatch watch(child.Identity());
  child.RequestExitLeavingDescendantHoldingOutput();
  ASSERT_TRUE(watch.WaitUntil(Clock::now() + 10s));
  EXPECT_EQ(child.Reap(), 0);
  EXPECT_FALSE(child.OutputHasEof(0));
  child.RequestExit();
  EXPECT_TRUE(child.OutputHasEof(10000));
}
#endif

TEST(ProcessIdentityProbeTest, MatchingSelfPreservesLiveObservation) {
  const auto identity = CurrentProcessIdentity();
  ProcessWatch watch(identity);
  EXPECT_EQ(ProbeProcessIdentity(identity), ProcessIdentityProbe::kMatching);
  EXPECT_FALSE(watch.IsExited());
  EXPECT_EQ(CurrentProcessIdentity(), identity);
}

TEST(ProcessIdentityProbeTest, ExitedAndReapedChildIsAbsentOrHasObservedOsExit) {
  ProcessIdentity identity;
  {
    Child child;
    identity = child.Identity();
    EXPECT_EQ(ProbeProcessIdentity(identity), ProcessIdentityProbe::kMatching);
    child.RequestExit();
    ASSERT_EQ(child.Reap(), 0);
  }

  // Other Windows components may retain the terminated process object after all fixture handles close.
  const auto deadline = Clock::now() + 10s;
  while (Clock::now() < deadline) {
    const auto probe = ProbeProcessIdentity(identity);
    if (probe == ProcessIdentityProbe::kAbsent) {
      return;
    }

    ASSERT_EQ(probe, ProcessIdentityProbe::kMatching);
    try {
      ProcessWatch watch(identity);
      EXPECT_EQ(watch.Identity(), identity);
      ASSERT_TRUE(watch.WaitUntil(deadline));
      return;
    } catch (const ProcessWatchError& error) {
      if (error.Failure() != ProcessWatchFailure::kStaleIdentity) {
        throw;
      }

      // Disappearance between probe and acquisition requires another positive probe, not success from this error.
    }
  }

  FAIL() << "Neither process absence nor OS exit was positively observed before the deadline";
}

TEST(ProcessIdentityProbeTest, MismatchedGenerationIsAbsentWhileRealChildRemainsAlive) {
  Child child;
  const auto identity = child.Identity();
  const ProcessIdentity predecessor{.pid = identity.pid, .generation = identity.generation == 1 ? 2u : 1u};
  ProcessWatch watch(identity);
  EXPECT_EQ(ProbeProcessIdentity(predecessor), ProcessIdentityProbe::kAbsent);
  EXPECT_EQ(ProbeProcessIdentity(identity), ProcessIdentityProbe::kMatching);
  EXPECT_FALSE(watch.IsExited());
  child.RequestExit();
  EXPECT_TRUE(watch.WaitUntil(Clock::now() + 10s));
  EXPECT_EQ(child.Reap(), 0);
}

TEST(ProcessIdentityProbeTest, InvalidIdentitiesThrowRatherThanReportAbsence) {
  const auto current = CurrentProcessIdentity();
  EXPECT_THROW((void)ProbeProcessIdentity({0, current.generation}), std::invalid_argument);
  EXPECT_THROW((void)ProbeProcessIdentity({current.pid, 0}), std::invalid_argument);
  EXPECT_THROW((void)ProbeProcessIdentity({std::numeric_limits<std::uint64_t>::max(), current.generation}),
               std::invalid_argument);
}

#ifdef __linux__
TEST(ProcessWatchTest, WatchDescriptorIsCloseOnExecAndReleasedByDestruction) {
  std::vector<int> descriptors;
  {
    ProcessWatch watch(CurrentProcessIdentity());
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
      std::error_code error;
      const auto target = std::filesystem::read_symlink(entry.path(), error);
      if (!error && target == "anon_inode:[pidfd]") {
        const auto fd = std::stoi(entry.path().filename().string());
        descriptors.push_back(fd);
        const auto flags = fcntl(fd, F_GETFD);
        ASSERT_GE(flags, 0);
        EXPECT_NE(flags & FD_CLOEXEC, 0);
      }
    }

    ASSERT_EQ(descriptors.size(), 1u);
  }

  errno = 0;
  EXPECT_EQ(fcntl(descriptors.front(), F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
}
#endif

}  // namespace
}  // namespace fl::hosting
