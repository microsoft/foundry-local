// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "hosting/process_watch.h"

#include <windows.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace fl::hosting {
namespace {

[[noreturn]] void Fail(const char* operation, DWORD error = GetLastError()) {
  throw ProcessWatchError(ProcessWatchFailure::kObservationFailed, {static_cast<int>(error), std::system_category()},
                          operation);
}

[[noreturn]] void Stale() {
  throw ProcessWatchError(ProcessWatchFailure::kStaleIdentity, std::make_error_code(std::errc::no_such_process),
                          "Process identity is stale or already exited");
}

void ValidateIdentity(ProcessIdentity identity) {
  if (identity.pid == 0 || identity.pid > std::numeric_limits<DWORD>::max() || identity.generation == 0) {
    throw std::invalid_argument("ProcessWatch requires a valid PID and nonzero generation");
  }
}

std::uint64_t CreationTime(HANDLE process) {
  FILETIME creation{}, exit{}, kernel{}, user{};
  if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
    Fail("GetProcessTimes");
  }

  return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}

DWORD WaitMilliseconds(ProcessWatch::TimePoint deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (deadline <= now) {
    return 0;
  }

  const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
  return static_cast<DWORD>(std::min<std::int64_t>(remaining, INFINITE - 1));
}

}  // namespace

struct ProcessWatch::State {
  ~State() {
    if (process != nullptr) {
      CloseHandle(process);
    }
  }

  HANDLE process{};
  bool exited{};
};

ProcessIdentity CurrentProcessIdentity() {
  return {.pid = GetCurrentProcessId(), .generation = CreationTime(GetCurrentProcess())};
}

ProcessIdentityProbe ProbeProcessIdentity(ProcessIdentity identity) {
  ValidateIdentity(identity);
  const std::unique_ptr<void, decltype(&CloseHandle)> process(
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(identity.pid)), &CloseHandle);
  if (!process) {
    const auto error = GetLastError();
    if (error == ERROR_INVALID_PARAMETER) {
      return ProcessIdentityProbe::kAbsent;
    }

    Fail("OpenProcess(identity probe)", error);
  }

  return CreationTime(process.get()) == identity.generation ? ProcessIdentityProbe::kMatching
                                                            : ProcessIdentityProbe::kAbsent;
}

ProcessWatch::ProcessWatch(ProcessIdentity identity) : identity_(identity), state_(std::make_unique<State>()) {
  ValidateIdentity(identity);
  state_->process =
      OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(identity.pid));
  if (state_->process == nullptr) {
    const auto error = GetLastError();
    if (error == ERROR_INVALID_PARAMETER) {
      Stale();
    }

    Fail("OpenProcess", error);
  }

  if (CreationTime(state_->process) != identity.generation) {
    Stale();
  }
}

ProcessWatch::~ProcessWatch() = default;
ProcessWatch::ProcessWatch(ProcessWatch&&) noexcept = default;
ProcessWatch& ProcessWatch::operator=(ProcessWatch&&) noexcept = default;

bool ProcessWatch::IsExited() { return WaitUntil(std::chrono::steady_clock::now()); }

bool ProcessWatch::WaitUntil(TimePoint deadline) {
  if (!state_) {
    throw std::logic_error("Cannot use a moved-from ProcessWatch");
  }

  while (!state_->exited) {
    const auto result = WaitForSingleObject(state_->process, WaitMilliseconds(deadline));
    if (result == WAIT_OBJECT_0) {
      state_->exited = true;
    } else if (result != WAIT_TIMEOUT) {
      Fail("WaitForSingleObject");
    } else if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
  }

  return true;
}

}  // namespace fl::hosting
#endif  // _WIN32
