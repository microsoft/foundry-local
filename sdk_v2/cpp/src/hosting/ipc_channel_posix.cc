// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "hosting/ipc_channel.h"

#include "exception.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/message.h>
#include <sys/acl.h>
#endif

namespace fl::hosting {
namespace {

[[noreturn]] void Fail(const char* operation, int error = errno) {
  FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, operation, " failed (errno ", error, ")");
}

class Descriptor {
 public:
  explicit Descriptor(int value = -1) noexcept : value_(value) {}
  ~Descriptor() {
    if (value_ >= 0) {
      close(value_);
    }
  }
  Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
  Descriptor& operator=(Descriptor&& other) noexcept {
    if (this != &other) {
      Descriptor old(std::exchange(value_, std::exchange(other.value_, -1)));
    }

    return *this;
  }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  int Get() const noexcept { return value_; }

 private:
  int value_;
};

#ifdef __APPLE__
void SetCloseOnExec(int fd) {
  // Darwin has no public socket/accept atomic CLOEXEC flag. This cannot close the concurrent fork/exec window.
  if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
    Fail("fcntl(FD_CLOEXEC)");
  }
}

void VerifyDirectoryAcl(int fd) {
  const auto acl = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
  if (acl == nullptr) {
    if (errno == ENOENT) {
      return;
    }

    Fail("acl_get_fd_np(endpoint directory)");
  }

  acl_entry_t entry{};
  const auto result = acl_get_entry(acl, ACL_FIRST_ENTRY, &entry);
  const auto error = errno;
  acl_free(acl);
  if (result == 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC endpoint directory must not have an extended ACL");
  }
  if (error != EINVAL) {
    Fail("acl_get_entry(endpoint directory)", error);
  }
}
#endif

void SetNonBlocking(int fd, bool enabled) {
  const auto flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) < 0) {
    Fail("fcntl(O_NONBLOCK)");
  }
}

std::pair<Descriptor, Descriptor> NewWakeupPipe() {
  std::array<int, 2> fds{};
#ifdef __linux__
  if (pipe2(fds.data(), O_CLOEXEC | O_NONBLOCK) < 0) {
    Fail("pipe2");
  }
#else
  // Like socket/accept, Darwin pipe creation has no public atomic CLOEXEC option.
  if (pipe(fds.data()) < 0) {
    Fail("pipe");
  }
#endif

  Descriptor reader(fds[0]);
  Descriptor writer(fds[1]);
#ifdef __APPLE__
  SetCloseOnExec(reader.Get());
  SetCloseOnExec(writer.Get());
  SetNonBlocking(reader.Get(), true);
  SetNonBlocking(writer.Get(), true);
#endif
  return {std::move(reader), std::move(writer)};
}

Descriptor NewSocket() {
#ifdef __linux__
  Descriptor socket_fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
#elif defined(__APPLE__)
  Descriptor socket_fd(socket(AF_UNIX, SOCK_STREAM, 0));
#else
#error Native IPC supports Linux and macOS only on POSIX.
#endif
  if (socket_fd.Get() < 0) {
    Fail("socket");
  }

#ifdef __APPLE__
  SetCloseOnExec(socket_fd.Get());
  const int enabled = 1;
  if (setsockopt(socket_fd.Get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
    Fail("setsockopt(SO_NOSIGPIPE)");
  }
#endif
  return socket_fd;
}

std::optional<Descriptor> AcceptSocket(int listener) {
  int fd = -1;
  do {
#ifdef __linux__
    fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
#else
    fd = accept(listener, nullptr, nullptr);
#endif
  } while (fd < 0 && errno == EINTR);

  if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    return std::nullopt;
  }

  Descriptor accepted(fd);
  if (fd < 0) {
    Fail("accept");
  }

#ifdef __APPLE__
  SetCloseOnExec(fd);
  SetNonBlocking(fd, false);
  const int enabled = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
    Fail("setsockopt(SO_NOSIGPIPE)");
  }
#endif
  return accepted;
}

Descriptor OpenDirectory(const std::filesystem::path& path) {
  Descriptor directory(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (directory.Get() < 0) {
    Fail("open(root)");
  }

  for (const auto& component : path.relative_path()) {
    const auto name = component.native();
    if (name.empty() || name == "." || name == "..") {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Native IPC endpoint must have a normalized absolute path");
    }

    Descriptor next(openat(directory.Get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (next.Get() < 0) {
      Fail("openat(endpoint directory)");
    }

    struct stat status{};
    if (fstat(next.Get(), &status) < 0) {
      Fail("fstat(endpoint directory)");
    }
    if ((status.st_uid != 0 && status.st_uid != geteuid()) ||
        ((status.st_mode & 0022) != 0 && (status.st_mode & S_ISVTX) == 0)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC directory ancestor is not trusted");
    }

    directory = std::move(next);
  }

  struct stat status{};
  if (fstat(directory.Get(), &status) < 0) {
    Fail("fstat(endpoint directory)");
  }
  if (status.st_uid != geteuid() || (status.st_mode & 0777) != 0700) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
             "Native IPC endpoint directory must be owned by this user with mode 0700");
  }

#ifdef __APPLE__
  VerifyDirectoryAcl(directory.Get());
#endif
  return directory;
}

struct Endpoint {
  explicit Endpoint(const std::filesystem::path& path) {
    const auto text = path.native();
    if (!path.is_absolute() || path.filename().empty() || path.filename() == "." || path.filename() == ".." ||
        text.find('\0') != std::string::npos || text.size() >= sizeof(address.sun_path)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Native IPC endpoint path is invalid or too long");
    }

    directory = OpenDirectory(path.parent_path());
    name = path.filename().native();
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
#ifdef __APPLE__
    address.sun_len = static_cast<unsigned char>(sizeof(address));
#endif
  }

  bool VerifySocket(bool allow_absent = false) const {
    struct stat status{};
    if (fstatat(directory.Get(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) < 0) {
      if (allow_absent && errno == ENOENT) {
        return false;
      }

      Fail("fstatat(endpoint)");
    }
    if (!S_ISSOCK(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & 0777) != 0600) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC endpoint must be an owner-only socket");
    }

    return true;
  }

  Descriptor directory;
  std::string name;
  sockaddr_un address{};
};

#if defined(__linux__) && defined(SO_PEERPIDFD)
std::optional<std::uint64_t> PeerGeneration(int socket_fd, pid_t pid) {
  int raw_pidfd = -1;
  socklen_t size = sizeof(raw_pidfd);
  if (getsockopt(socket_fd, SOL_SOCKET, SO_PEERPIDFD, &raw_pidfd, &size) < 0) {
    return std::nullopt;
  }

  const Descriptor pidfd(raw_pidfd);
  const auto path = "/proc/" + std::to_string(pid) + "/stat";
  const Descriptor file(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (file.Get() < 0) {
    return std::nullopt;
  }

  std::array<char, 4096> buffer{};
  const auto count = read(file.Get(), buffer.data(), buffer.size());
  if (count <= 0 || static_cast<std::size_t>(count) == buffer.size()) {
    return std::nullopt;
  }

  const std::string text(buffer.data(), static_cast<std::size_t>(count));
  const auto end_name = text.rfind(')');
  if (end_name == std::string::npos) {
    return std::nullopt;
  }

  std::istringstream fields(text.substr(end_name + 1));
  std::string ignored;
  for (int field = 3; field < 22; ++field) {
    if (!(fields >> ignored)) {
      return std::nullopt;
    }
  }

  std::uint64_t start_time = 0;
  pollfd live{pidfd.Get(), POLLIN, 0};
  if (!(fields >> start_time) || poll(&live, 1, 0) != 0) {
    return std::nullopt;
  }

  return start_time;
}
#endif

PeerIdentity Authenticate(int fd) {
#ifdef __linux__
  ucred credentials{};
  socklen_t size = sizeof(credentials);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) < 0) {
    Fail("getsockopt(SO_PEERCRED)");
  }
  if (size != sizeof(credentials) || credentials.uid != geteuid() || credentials.pid <= 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC peer credentials are invalid or belong to another user");
  }

  PeerIdentity identity{static_cast<std::uint64_t>(credentials.pid), std::nullopt};
#ifdef SO_PEERPIDFD
  identity.generation = PeerGeneration(fd, credentials.pid);
#endif
  // Older kernels/headers cannot bind /proc start time to the socket peer safely across PID reuse.
  return identity;
#else
  uid_t uid = 0;
  gid_t gid = 0;
  pid_t pid = 0;
  socklen_t size = sizeof(pid);
  if (getpeereid(fd, &uid, &gid) < 0 || getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &size) < 0) {
    Fail("Unix socket peer credentials");
  }
  if (uid != geteuid() || size != sizeof(pid) || pid <= 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC peer credentials are invalid or belong to another user");
  }

  PeerIdentity identity{static_cast<std::uint64_t>(pid), std::nullopt};
#ifdef LOCAL_PEERTOKEN
  audit_token_t token{};
  size = sizeof(token);
  if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERTOKEN, &token, &size) == 0 && size == sizeof(token)) {
    if (token.val[1] != uid || token.val[5] != static_cast<std::uint32_t>(pid)) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC peer audit token does not match credentials");
    }

    identity.generation = token.val[7];
  }
#endif
  return identity;
#endif
}

}  // namespace

struct Channel::State {
  State(Descriptor descriptor, PeerIdentity identity) : fd(std::move(descriptor)), peer(identity) {}
  Descriptor fd;
  PeerIdentity peer;
  std::mutex read_mutex;
  std::mutex write_mutex;
  std::atomic<bool> closed{};
};

struct Listener::State {
  explicit State(const std::filesystem::path& path) : endpoint(path), fd(NewSocket()), wakeup(NewWakeupPipe()) {
    SetNonBlocking(fd.Get(), true);
  }

  ~State() { Close(); }

  void RequireOpenLocked() const {
    if (closed) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC listener is closed");
    }
  }

  void CleanupEndpoint() noexcept {
    struct stat current{};
    if (owned && fstatat(endpoint.directory.Get(), endpoint.name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISSOCK(current.st_mode) && current.st_uid == geteuid() && current.st_dev == identity.st_dev &&
        current.st_ino == identity.st_ino) {
      unlinkat(endpoint.directory.Get(), endpoint.name.c_str(), 0);
    }

    owned = false;
  }

  void Close() noexcept {
    const std::lock_guard lock(mutex);
    if (closed) {
      return;
    }

    closed = true;
    const std::byte signal{1};
    while (write(wakeup.second.Get(), &signal, 1) < 0 && errno == EINTR) {
    }

    CleanupEndpoint();
    if (!accepting) {
      fd = Descriptor{};
    }
  }

  struct AcceptLease {
    explicit AcceptLease(State& listener) : state(listener) {
      const std::lock_guard lock(state.mutex);
      state.RequireOpenLocked();
      state.accepting = true;
      fd = state.fd.Get();
    }

    ~AcceptLease() {
      const std::lock_guard lock(state.mutex);
      state.accepting = false;
      // The poll/accept fd stays valid until the active accept has stopped using its captured descriptor.
      if (state.closed) {
        state.fd = Descriptor{};
      }
    }

    AcceptLease(const AcceptLease&) = delete;
    AcceptLease& operator=(const AcceptLease&) = delete;

    void Wait() const {
      std::array<pollfd, 2> fds{{{fd, POLLIN, 0}, {state.wakeup.first.Get(), POLLIN, 0}}};
      int result = 0;
      do {
        result = poll(fds.data(), static_cast<nfds_t>(fds.size()), -1);
      } while (result < 0 && errno == EINTR);

      if (result < 0) {
        Fail("poll(listener)");
      }

      const std::lock_guard lock(state.mutex);
      state.RequireOpenLocked();
      if ((fds[0].revents & POLLIN) == 0 || fds[1].revents != 0) {
        Fail("poll(listener readiness)", EIO);
      }
    }

    State& state;
    int fd{-1};
  };

  Endpoint endpoint;
  Descriptor fd;
  std::pair<Descriptor, Descriptor> wakeup;
  struct stat identity{};
  bool owned{};
  bool closed{};
  bool accepting{};
  std::mutex accept_mutex;
  std::mutex mutex;
};

std::mutex& Channel::ReadMutex(State& state) { return state.read_mutex; }
std::mutex& Channel::WriteMutex(State& state) { return state.write_mutex; }

void Channel::ReadExact(State& state, std::span<std::byte> bytes) {
  while (!bytes.empty()) {
    if (state.closed.load()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC channel is closed");
    }

    const auto count = recv(state.fd.Get(), bytes.data(), bytes.size(), 0);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Native IPC read failed, EOF or truncated frame (errno ", errno, ")");
    }

    bytes = bytes.subspan(static_cast<std::size_t>(count));
  }
}

void Channel::WriteExact(State& state, std::span<const std::byte> bytes) {
  while (!bytes.empty()) {
    if (state.closed.load()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC channel is closed");
    }

#ifdef __linux__
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    const auto count = send(state.fd.Get(), bytes.data(), bytes.size(), flags);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      Fail("send");
    }

    bytes = bytes.subspan(static_cast<std::size_t>(count));
  }
}

void Channel::Close() noexcept {
  if (state_ && !state_->closed.exchange(true)) {
    // shutdown wakes both directions without closing an fd still used by a reader/writer.
    shutdown(state_->fd.Get(), SHUT_RDWR);
  }
}

PeerIdentity Channel::Peer() const { return RequireState()->peer; }

Listener::Listener(const std::filesystem::path& endpoint) : state_(std::make_unique<State>(endpoint)) {
  const auto& address = state_->endpoint.address;
  if (bind(state_->fd.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
    Fail("bind");
  }

  if (fstatat(state_->endpoint.directory.Get(), state_->endpoint.name.c_str(), &state_->identity, AT_SYMLINK_NOFOLLOW) <
      0) {
    Fail("fstatat(bound endpoint)");
  }

  state_->owned = S_ISSOCK(state_->identity.st_mode) && state_->identity.st_uid == geteuid();
  if (!state_->owned) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC endpoint ownership changed during bind");
  }
  if (fchmodat(state_->endpoint.directory.Get(), state_->endpoint.name.c_str(), 0600, 0) < 0) {
    Fail("fchmodat(endpoint)");
  }

  state_->endpoint.VerifySocket();
  if (listen(state_->fd.Get(), 16) < 0) {
    Fail("listen");
  }
}

Listener::~Listener() = default;
Listener::Listener(Listener&& other) noexcept = default;
Listener& Listener::operator=(Listener&& other) noexcept = default;

void Listener::Close() noexcept {
  if (state_) {
    state_->Close();
  }
}

std::pair<Channel, PeerIdentity> Listener::Accept() {
  if (!state_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC listener is empty");
  }

  const std::lock_guard accept_lock(state_->accept_mutex);
  const State::AcceptLease lease(*state_);
  while (true) {
    lease.Wait();
    auto fd = AcceptSocket(lease.fd);
    if (!fd) {
      continue;
    }

    const auto peer = Authenticate(fd->Get());
    auto channel_state = std::make_shared<Channel::State>(std::move(*fd), peer);
    const std::lock_guard lock(state_->mutex);
    state_->RequireOpenLocked();
    return {Channel(std::move(channel_state)), peer};
  }
}

Channel Connect(const std::filesystem::path& endpoint) {
  auto channel = TryConnect(endpoint);
  if (!channel) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Native IPC endpoint is absent or not accepting connections");
  }

  return std::move(*channel);
}

std::optional<Channel> TryConnect(const std::filesystem::path& endpoint) {
  const Endpoint address(endpoint);
  if (!address.VerifySocket(true)) {
    return std::nullopt;
  }
  auto fd = NewSocket();
  if (connect(fd.Get(), reinterpret_cast<const sockaddr*>(&address.address), sizeof(address.address)) < 0) {
    if (errno == ENOENT || errno == ECONNREFUSED) {
      return std::nullopt;
    }

    Fail("connect");
  }

  const auto peer = Authenticate(fd.Get());
  return Channel(std::make_shared<Channel::State>(std::move(fd), peer));
}

}  // namespace fl::hosting
#endif
