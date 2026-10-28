// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hosting/host_process.h"

#include "exception.h"
#include "util/scope_guard.h"

#ifdef _WIN32
#include "hosting/windows_user.h"
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fl::hosting {

std::optional<ProcessIdentity> SpawnNativeHost(const std::filesystem::path& executable,
                                               const std::filesystem::path& bootstrap) {
  if (!executable.is_absolute() || !bootstrap.is_absolute()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "native host launch paths must be absolute");
  }
#ifdef _WIN32
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

  auto program = executable.string();
  auto descriptor = bootstrap.string();
  char option[] = "--bootstrap";
  char detach[] = "--detach";
  char* arguments[]{program.data(), option, descriptor.data(), detach, nullptr};
  pid_t launcher{};
  error = posix_spawn(&launcher, executable.c_str(), &actions, nullptr, arguments, environ);
  if (error != 0) {
    throw HostLaunchRejected(error, std::generic_category(), "start native host");
  }

  // The thin launcher forks before loading any native modules and exits immediately. Reap it synchronously;
  // the daemon publishes its own generation, and no SDK thread is left running after a client unloads the SDK.
  int status{};
  while (waitpid(launcher, &status, 0) < 0) {
    if (errno != EINTR) {
      throw std::system_error(errno, std::generic_category(), "reap native host launcher");
    }
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "native host launcher failed; preserving pending reconciliation state");
  }

  return std::nullopt;
#endif
}

}  // namespace fl::hosting
