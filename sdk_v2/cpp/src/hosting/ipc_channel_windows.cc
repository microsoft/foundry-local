// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/ipc_channel.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "exception.h"
#include "hosting/windows_user.h"

#include <algorithm>
#include <string>
#include <system_error>

namespace fl::hosting {
namespace {

[[noreturn]] void Fail(const char* operation, DWORD error = GetLastError()) {
  FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, operation, " failed (Windows error ", error, ")");
}

class Handle {
 public:
  explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
  ~Handle() { Reset(); }
  Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) {
      Reset();
      value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
    }

    return *this;
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HANDLE Get() const noexcept { return value_; }
  void Reset() noexcept {
    if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
      CloseHandle(value_);
    }

    value_ = INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE value_;
};

using windows::OwnerSecurity;
using windows::ProcessUser;
using windows::UserSid;

std::wstring PipeName(const std::filesystem::path& endpoint) {
  const auto name = endpoint.native();
  constexpr std::wstring_view prefix = LR"(\\.\pipe\)";
  if (!name.starts_with(prefix) || name.size() == prefix.size() || name.size() > 256 ||
      name.find(L'\\', prefix.size()) != std::wstring::npos || name.find(L'\0') != std::wstring::npos) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Native IPC requires a full local named-pipe name");
  }

  return name;
}

struct Pipe {
  explicit Pipe(Handle handle_value) : handle(std::move(handle_value)) {}
  ~Pipe() { Close(); }

  void Close() noexcept {
    const std::lock_guard lock(mutex);
    if (!closed) {
      closed = true;
      CancelIoEx(handle.Get(), nullptr);
      if (active == 0) {
        handle.Reset();
      }
    }
  }

  Handle handle;
  std::mutex mutex;
  bool closed{};
  std::size_t active{};
};

class IoLease {
 public:
  explicit IoLease(Pipe& pipe) : pipe_(pipe) {
    const std::lock_guard lock(pipe_.mutex);
    if (pipe_.closed) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC channel is closed");
    }

    ++pipe_.active;
    handle_ = pipe_.handle.Get();
  }

  ~IoLease() {
    const std::lock_guard lock(pipe_.mutex);
    if (--pipe_.active == 0 && pipe_.closed) {
      pipe_.handle.Reset();
    }
  }

  IoLease(const IoLease&) = delete;
  IoLease& operator=(const IoLease&) = delete;
  HANDLE Get() const noexcept { return handle_; }

 private:
  Pipe& pipe_;
  HANDLE handle_{};
};

template <typename Start>
DWORD Perform(Pipe& pipe, Start start) {
  const Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (event.Get() == nullptr) {
    Fail("CreateEvent");
  }

  OVERLAPPED operation{};
  operation.hEvent = event.Get();
  const IoLease lease(pipe);
  DWORD error = ERROR_SUCCESS;
  {
    // Close and submission share a lock: cancellation must never miss a newly submitted request.
    const std::lock_guard lock(pipe.mutex);
    if (pipe.closed) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC channel is closed");
    }

    if (!start(lease.Get(), operation)) {
      error = GetLastError();
    }
  }

  if (error == ERROR_PIPE_CONNECTED) {
    return 0;
  }
  if (error != ERROR_SUCCESS && error != ERROR_IO_PENDING) {
    Fail("Native IPC I/O", error);
  }

  DWORD transferred = 0;
  // Cancellation is only a request. Buffers, OVERLAPPED, and the handle live until completion is observed.
  if (!GetOverlappedResult(lease.Get(), &operation, &transferred, TRUE)) {
    Fail("GetOverlappedResult");
  }

  return transferred;
}

std::shared_ptr<Pipe> CreatePipe(const std::wstring& name, bool first) {
  const auto descriptor = OwnerSecurity();
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
  const DWORD flags = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
  Handle handle(CreateNamedPipeW(name.c_str(), flags, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                 PIPE_UNLIMITED_INSTANCES, static_cast<DWORD>(kMaxFramePayload),
                                 static_cast<DWORD>(kMaxFramePayload), 0, &security));
  if (handle.Get() == INVALID_HANDLE_VALUE) {
    Fail("CreateNamedPipe");
  }

  return std::make_shared<Pipe>(std::move(handle));
}

struct AuthenticatedPeer {
  PeerIdentity identity;
  Handle process;
};

AuthenticatedPeer Authenticate(HANDLE pipe, bool server) {
  ULONG pid = 0;
  const auto query_pid = server ? GetNamedPipeClientProcessId : GetNamedPipeServerProcessId;
  if (!query_pid(pipe, &pid) || pid == 0) {
    Fail("GetNamedPipe peer process ID");
  }

  Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid));
  if (process.Get() == nullptr) {
    Fail("OpenProcess(peer)");
  }

  const auto own_user = ProcessUser(GetCurrentProcess());
  const auto peer_user = ProcessUser(process.Get());
  if (!EqualSid(UserSid(own_user), UserSid(peer_user))) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC peer belongs to another user");
  }

  FILETIME created{}, exited{}, kernel{}, user{};
  ULONG verified_pid = 0;
  if (!GetProcessTimes(process.Get(), &created, &exited, &kernel, &user) || !query_pid(pipe, &verified_pid) ||
      verified_pid != pid || WaitForSingleObject(process.Get(), 0) != WAIT_TIMEOUT) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC peer identity changed or exited");
  }

  const auto generation = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  return {{pid, generation}, std::move(process)};
}

}  // namespace

struct Channel::State {
  std::shared_ptr<Pipe> pipe;
  AuthenticatedPeer peer;
  std::mutex read_mutex;
  std::mutex write_mutex;
};

struct Listener::State {
  std::wstring name;
  std::shared_ptr<Pipe> pending;
  std::mutex accept_mutex;
  std::mutex mutex;
  bool closed{};

  ~State() { Close(); }

  void RequireOpenLocked() const {
    if (closed) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC listener is closed");
    }
  }

  void Close() noexcept {
    const std::lock_guard lock(mutex);
    closed = true;
    if (pending) {
      pending->Close();
      pending.reset();
    }
  }
};

std::mutex& Channel::ReadMutex(State& state) { return state.read_mutex; }
std::mutex& Channel::WriteMutex(State& state) { return state.write_mutex; }

void Channel::ReadExact(State& state, std::span<std::byte> bytes) {
  while (!bytes.empty()) {
    const auto count = Perform(*state.pipe, [&](HANDLE pipe, OVERLAPPED& operation) {
      return ReadFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, &operation);
    });
    if (count == 0) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Native IPC EOF or truncated frame");
    }

    bytes = bytes.subspan(count);
  }
}

void Channel::WriteExact(State& state, std::span<const std::byte> bytes) {
  while (!bytes.empty()) {
    const auto count = Perform(*state.pipe, [&](HANDLE pipe, OVERLAPPED& operation) {
      return WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, &operation);
    });
    if (count == 0) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Native IPC write made no progress");
    }

    bytes = bytes.subspan(count);
  }
}

void Channel::Close() noexcept {
  if (state_) {
    state_->pipe->Close();
  }
}

PeerIdentity Channel::Peer() const { return RequireState()->peer.identity; }

Listener::Listener(const std::filesystem::path& endpoint) : state_(std::make_unique<State>()) {
  state_->name = PipeName(endpoint);
  state_->pending = CreatePipe(state_->name, true);
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
  for (;;) {
    std::shared_ptr<Pipe> pipe;
    {
      const std::lock_guard lock(state_->mutex);
      state_->RequireOpenLocked();
      pipe = state_->pending;
    }

    std::shared_ptr<Channel::State> channel_state;
    try {
      Perform(*pipe, [](HANDLE handle, OVERLAPPED& operation) { return ConnectNamedPipe(handle, &operation); });
      const IoLease lease(*pipe);
      auto peer = Authenticate(lease.Get(), true);
      channel_state = std::make_shared<Channel::State>();
      channel_state->pipe = pipe;
      channel_state->peer = std::move(peer);
    } catch (const Exception&) {
      // A disconnected or rejected peer retires only this pipe instance, not the shared listener.
    } catch (const std::system_error&) {
      // Token queries can also fail if the peer exits during authentication.
    }

    const std::lock_guard lock(state_->mutex);
    state_->RequireOpenLocked();
    // Keep an instance alive continuously; a second listener must never acquire the name between accepts.
    state_->pending = CreatePipe(state_->name, false);
    if (channel_state) {
      const auto identity = channel_state->peer.identity;
      return {Channel(std::move(channel_state)), identity};
    }
  }
}

Channel Connect(const std::filesystem::path& endpoint) {
  auto channel = TryConnect(endpoint);
  if (!channel) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL, "Native IPC endpoint is absent or not accepting connections");
  }

  return std::move(*channel);
}

std::optional<Channel> TryConnect(const std::filesystem::path& endpoint, std::chrono::steady_clock::time_point) {
  const auto name = PipeName(endpoint);
  Handle handle(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
  if (handle.Get() == INVALID_HANDLE_VALUE) {
    const auto error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_PIPE_BUSY) {
      return std::nullopt;
    }

    Fail("CreateFile(named pipe)", error);
  }

  auto peer = Authenticate(handle.Get(), false);
  auto state = std::make_shared<Channel::State>();
  state->pipe = std::make_shared<Pipe>(std::move(handle));
  state->peer = std::move(peer);
  return Channel(std::move(state));
}

}  // namespace fl::hosting
#endif
