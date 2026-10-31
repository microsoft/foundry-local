// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#ifndef _WIN32
#include "hosting/process_watch.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <utility>

#ifdef __linux__
#include <poll.h>
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <sys/event.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#endif

namespace fl::hosting {
namespace {

[[noreturn]] void Fail(const char* operation, int error = errno) {
  const auto failure =
      error == ENOSYS || error == ENOTSUP ? ProcessWatchFailure::kUnsupported : ProcessWatchFailure::kObservationFailed;
  throw ProcessWatchError(failure, {error, std::generic_category()}, operation);
}

[[noreturn]] void Stale() {
  throw ProcessWatchError(ProcessWatchFailure::kStaleIdentity, std::make_error_code(std::errc::no_such_process),
                          "Process identity is stale or already exited");
}

void ValidateIdentity(ProcessIdentity identity) {
  if (identity.pid == 0 || identity.pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()) ||
      identity.generation == 0) {
    throw std::invalid_argument("ProcessWatch requires a valid PID and nonzero generation");
  }
}

struct GenerationSnapshot {
  std::uint64_t generation;
  bool exiting{};
};

class Descriptor {
 public:
  explicit Descriptor(int fd) noexcept : fd_(fd) {}
  ~Descriptor() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  [[nodiscard]] int Get() const noexcept { return fd_; }
  [[nodiscard]] int Release() noexcept { return std::exchange(fd_, -1); }

 private:
  int fd_;
};

#ifdef __linux__
std::uint64_t ParseGeneration(std::string_view stat) {
  // comm may contain spaces, newlines and parentheses; field 3 starts after the LAST closing parenthesis.
  const auto end = stat.rfind(')');
  if (end == std::string_view::npos || end + 2 >= stat.size()) {
    Fail("Malformed /proc/PID/stat", EIO);
  }

  auto fields = stat.substr(end + 2);
  for (int field = 3; field < 22; ++field) {
    const auto space = fields.find(' ');
    if (space == std::string_view::npos) {
      Fail("Missing /proc/PID/stat starttime", EIO);
    }

    fields.remove_prefix(space + 1);
  }

  const auto token = fields.substr(0, fields.find(' '));
  std::uint64_t generation{};
  const auto [last, error] = std::from_chars(token.data(), token.data() + token.size(), generation);
  if (error != std::errc{} || last != token.data() + token.size() || generation == 0) {
    Fail("Invalid /proc/PID/stat starttime", EIO);
  }

  return generation;
}

std::optional<std::string> ReadProcessStat(int fd) {
  std::array<char, 4096> buffer{};
  std::size_t used{};
  while (used < buffer.size()) {
    const auto size = read(fd, buffer.data() + used, buffer.size() - used);
    if (size < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == ESRCH) {
        return std::nullopt;
      }

      Fail("read(/proc/PID/stat)");
    }
    if (size == 0) {
      return std::string(buffer.data(), used);
    }

    used += static_cast<std::size_t>(size);
  }

  Fail("Oversized or incomplete /proc/PID/stat", EOVERFLOW);
}

std::optional<GenerationSnapshot> TryReadGeneration(pid_t pid) {
  const auto path = "/proc/" + std::to_string(pid) + "/stat";
  Descriptor file(open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (file.Get() < 0) {
    if (errno == ENOENT || errno == ESRCH) {
      return std::nullopt;
    }

    Fail("open(/proc/PID/stat)");
  }

  const auto stat = ReadProcessStat(file.Get());
  if (!stat) {
    return std::nullopt;
  }
  if (stat->empty() || stat->back() != '\n') {
    Fail("Incomplete /proc/PID/stat record", EIO);
  }

  return GenerationSnapshot{.generation = ParseGeneration(*stat)};
}

int TryOpenObserver(pid_t pid) {
#if defined(SYS_pidfd_open) || defined(__x86_64__) || defined(__aarch64__)
#ifdef SYS_pidfd_open
  constexpr auto kPidfdOpen = SYS_pidfd_open;
#else
  // The supported Linux ABIs use syscall 434, including when manylinux's older headers omit its name.
  constexpr long kPidfdOpen = 434;
#endif
  const auto fd = static_cast<int>(syscall(kPidfdOpen, pid, 0));
  if (fd < 0) {
    if (errno == ESRCH) {
      return -1;
    }

    Fail("pidfd_open (Linux 5.3+ required)");
  }

  return fd;
#else
  (void)pid;
  Fail("pidfd_open is unavailable in the build headers", ENOSYS);
#endif
}

int OpenObserver(pid_t pid) {
  const auto fd = TryOpenObserver(pid);
  if (fd < 0) {
    Stale();
  }

  return fd;
}

bool ObserveExit(int fd, ProcessWatch::TimePoint deadline) {
  const auto now = std::chrono::steady_clock::now();
  const auto remaining = deadline <= now ? 0 : std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
  pollfd event{.fd = fd, .events = POLLIN, .revents = 0};
  const auto result = poll(&event, 1, static_cast<int>(std::min<std::int64_t>(remaining, INT_MAX)));
  if (result < 0) {
    if (errno == EINTR) {
      return false;
    }

    Fail("poll(pidfd)");
  }

  if ((event.revents & (POLLERR | POLLNVAL)) != 0) {
    Fail("Invalid pidfd poll event", EIO);
  }

  return (event.revents & (POLLIN | POLLHUP)) != 0;
}
#elif defined(__APPLE__)
std::optional<GenerationSnapshot> TryReadGeneration(pid_t pid) {
  proc_bsdinfo info{};
  constexpr auto kInfoSize = static_cast<int>(sizeof(info));
  errno = 0;
  const auto size = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, kInfoSize);
  if (size != kInfoSize) {
    if (size == 0 && errno == ESRCH) {
      return std::nullopt;
    }

    Fail("proc_pidinfo(PROC_PIDTBSDINFO)", size == 0 && errno != 0 ? errno : EIO);
  }

  constexpr std::uint64_t kMicroseconds = 1000000;
  if (info.pbi_start_tvsec > std::numeric_limits<std::uint64_t>::max() / kMicroseconds - 1 ||
      info.pbi_start_tvusec >= kMicroseconds) {
    Fail("Invalid proc_pidinfo creation time", EIO);
  }

  const auto generation = info.pbi_start_tvsec * kMicroseconds + info.pbi_start_tvusec;
  if (generation == 0) {
    Fail("Invalid proc_pidinfo creation time", EIO);
  }

  return GenerationSnapshot{.generation = generation,
                            .exiting = info.pbi_status == SZOMB || (info.pbi_flags & PROC_FLAG_INEXIT) != 0};
}

int OpenObserver(pid_t pid) {
  Descriptor queue(kqueue());
  if (queue.Get() < 0) {
    Fail("kqueue");
  }

  // Darwin creates kqueues CLOEXEC atomically. Refuse an unexpected kernel instead of racing fork/exec with fcntl.
  const auto flags = fcntl(queue.Get(), F_GETFD);
  if (flags < 0) {
    Fail("fcntl(kqueue, F_GETFD)");
  }
  if ((flags & FD_CLOEXEC) == 0) {
    Fail("Kernel kqueue lacks atomic close-on-exec", ENOTSUP);
  }

  struct kevent change{};
  EV_SET(&change, static_cast<uintptr_t>(pid), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
  if (kevent(queue.Get(), &change, 1, nullptr, 0, nullptr) < 0) {
    if (errno == ESRCH) {
      Stale();
    }

    Fail("kevent(NOTE_EXIT registration)");
  }

  return queue.Release();
}

bool ObserveExit(int fd, ProcessWatch::TimePoint deadline) {
  const auto now = std::chrono::steady_clock::now();
  const auto remaining = deadline <= now ? ProcessWatch::TimePoint::duration::zero() : deadline - now;
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining - seconds);
  const timespec timeout{.tv_sec = static_cast<time_t>(seconds.count()), .tv_nsec = static_cast<long>(nanos.count())};
  struct kevent event{};
  const auto result = kevent(fd, nullptr, 0, &event, 1, &timeout);
  if (result < 0) {
    if (errno == EINTR) {
      return false;
    }

    Fail("kevent(NOTE_EXIT wait)");
  }

  if (result == 0) {
    return false;
  }
  if ((event.flags & EV_ERROR) != 0) {
    Fail("kevent(NOTE_EXIT event)", static_cast<int>(event.data));
  }
  if (event.filter != EVFILT_PROC || (event.fflags & NOTE_EXIT) == 0) {
    Fail("Unexpected kqueue process event", EIO);
  }

  return true;
}
#else
std::optional<GenerationSnapshot> TryReadGeneration(pid_t) {
  Fail("Exact process observation is unsupported on this platform", ENOTSUP);
}
int OpenObserver(pid_t) { Fail("Exact process observation is unsupported on this platform", ENOTSUP); }
bool ObserveExit(int, ProcessWatch::TimePoint) {
  Fail("Exact process observation is unsupported on this platform", ENOTSUP);
}
#endif

std::uint64_t ReadGeneration(pid_t pid) {
  const auto snapshot = TryReadGeneration(pid);
  // NOTE_EXIT is edge-triggered: reject an in-progress exit rather than accept a possibly missed registration edge.
  if (!snapshot || snapshot->exiting) {
    Stale();
  }

  return snapshot->generation;
}

}  // namespace

struct ProcessWatch::State {
  ~State() {
    if (fd >= 0) {
      close(fd);
    }
  }

  int fd{-1};
  bool exited{};
};

ProcessIdentity CurrentProcessIdentity() {
  return {.pid = static_cast<std::uint64_t>(getpid()), .generation = ReadGeneration(getpid())};
}

ProcessIdentityProbe ProbeProcessIdentity(ProcessIdentity identity) {
  ValidateIdentity(identity);
  const auto pid = static_cast<pid_t>(identity.pid);
#ifdef __linux__
  Descriptor process(TryOpenObserver(pid));
  if (process.Get() < 0) {
    return ProcessIdentityProbe::kAbsent;
  }

  const auto snapshot = TryReadGeneration(pid);
  if (!snapshot) {
    // Missing /proc entries can mean a hidden/unmounted procfs, not a missing OS process.
    Descriptor recheck(TryOpenObserver(pid));
    if (recheck.Get() < 0) {
      return ProcessIdentityProbe::kAbsent;
    }

    Fail("Process exists but its generation cannot be read", EIO);
  }
#else
  const auto snapshot = TryReadGeneration(pid);
  if (!snapshot) {
    return ProcessIdentityProbe::kAbsent;
  }
#endif
  return snapshot->generation == identity.generation ? ProcessIdentityProbe::kMatching : ProcessIdentityProbe::kAbsent;
}

ProcessWatch::ProcessWatch(ProcessIdentity identity) : identity_(identity), state_(std::make_unique<State>()) {
  ValidateIdentity(identity);
  const auto pid = static_cast<pid_t>(identity.pid);
  if (ReadGeneration(pid) != identity.generation) {
    Stale();
  }

  state_->fd = OpenObserver(pid);
  // Exit/reuse between the first read and acquisition must never bind us to a successor.
  if (ReadGeneration(pid) != identity.generation) {
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

  do {
    if (state_->exited || ObserveExit(state_->fd, deadline)) {
      state_->exited = true;
      return true;
    }
  } while (std::chrono::steady_clock::now() < deadline);

  return false;
}

}  // namespace fl::hosting
#endif  // !_WIN32
