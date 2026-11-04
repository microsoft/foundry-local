// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/host_process.h"

#include "exception.h"
#include "util/scope_guard.h"

#include <thread>
#ifdef _WIN32
#include "hosting/windows_user.h"
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fl::hosting {

std::optional<ProcessIdentity> SpawnNativeHost(const std::filesystem::path& executable,
                                               const std::filesystem::path& bootstrap,
                                               const HostStartupLock& startup_lock, ProcessWatch::TimePoint deadline) {
  if (!executable.is_absolute() || !bootstrap.is_absolute()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host launch paths must be absolute");
  }
#ifdef _WIN32
  (void)startup_lock;
  (void)deadline;
  // Both arguments are filenames, not directory names; Windows filenames cannot contain a quote.
  auto command = L"\"" + executable.wstring() + L"\" --bootstrap \"" + bootstrap.wstring() + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                      DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process)) {
    throw HostLaunchRejected(static_cast<int>(GetLastError()), std::system_category(), "start native host");
  }

  ScopeGuard close([&]() noexcept {
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  });
  FILETIME creation{}, exit{}, kernel{}, user{};
  windows::Check(GetProcessTimes(process.hProcess, &creation, &exit, &kernel, &user) != FALSE,
                 "capture launched native host process");
  return ProcessIdentity{process.dwProcessId, (uint64_t{creation.dwHighDateTime} << 32) | creation.dwLowDateTime};
#else
  const int lease = fcntl(startup_lock.Descriptor(), F_DUPFD_CLOEXEC, 3);
  if (lease < 0) {
    throw HostLaunchRejected(errno, std::generic_category(), "duplicate native host startup lease");
  }

  ScopeGuard release_lease([&]() noexcept { close(lease); });
  posix_spawn_file_actions_t actions;
  auto error = posix_spawn_file_actions_init(&actions);
  if (error != 0) {
    throw HostLaunchRejected(error, std::generic_category(), "initialize native host spawn");
  }

  ScopeGuard release([&]() noexcept { posix_spawn_file_actions_destroy(&actions); });
  for (int fd = 0; fd < 3; ++fd) {
    error = posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd == 0 ? O_RDONLY : O_WRONLY, 0);
    if (error != 0) {
      throw HostLaunchRejected(error, std::generic_category(), "prepare native host standard descriptors");
    }
  }

  error = posix_spawn_file_actions_adddup2(&actions, lease, 3);
  if (error == 0 && lease != 3) {
    error = posix_spawn_file_actions_addclose(&actions, lease);
  }
#ifdef __APPLE__
  posix_spawnattr_t attributes;
  if (error == 0) {
    error = posix_spawnattr_init(&attributes);
  }
  if (error != 0) {
    throw HostLaunchRejected(error, std::generic_category(), "prepare native host descriptor inheritance");
  }

  ScopeGuard release_attributes([&]() noexcept { posix_spawnattr_destroy(&attributes); });
  error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
  if (error != 0) {
    throw HostLaunchRejected(error, std::generic_category(), "prepare native host descriptor inheritance");
  }

  auto program = executable.string();
  auto descriptor = bootstrap.string();
  char option[] = "--bootstrap";
  char detach[] = "--detach";
  char* arguments[]{program.data(), option, descriptor.data(), detach, nullptr};
  pid_t launcher{};
#ifdef __linux__
  error = posix_spawn(&launcher, executable.c_str(), &actions, nullptr, arguments, environ);
#else
  error = posix_spawn(&launcher, executable.c_str(), &actions, &attributes, arguments, environ);
#endif
  if (error != 0) {
    throw HostLaunchRejected(error, std::generic_category(), "start native host");
  }

  // Only the unreaped launcher belongs to this process; never terminate an uncertain daemon.
  int status{};
  for (;;) {
    const auto result = waitpid(launcher, &status, WNOHANG);
    if (result == launcher) {
      break;
    }
    if (result < 0 && errno != EINTR) {
      throw std::system_error(errno, std::generic_category(), "reap native host launcher");
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      if (kill(launcher, SIGKILL) < 0 && errno != ESRCH) {
        throw std::system_error(errno, std::generic_category(), "stop timed-out native host launcher");
      }
      while (waitpid(launcher, &status, 0) < 0 && errno == EINTR) {
      }

      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "native host launcher exceeded its startup deadline");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "native host launcher failed; preserving pending reconciliation state");
  }

  return std::nullopt;
#endif
}

}  // namespace fl::hosting
