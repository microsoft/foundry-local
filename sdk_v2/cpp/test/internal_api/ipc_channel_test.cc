// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/ipc_channel.h"
#include "hosting/process_watch.h"
#include "util/scope_guard.h"
#include "utils/temp_path.h"

#include "exception.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <membership.h>
#include <sys/acl.h>
#endif

namespace fl::hosting {
namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

std::uint64_t CurrentPid() {
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return static_cast<std::uint64_t>(getpid());
#endif
}

class IpcChannelTest : public testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<unsigned> sequence{};
    const auto suffix = std::to_string(CurrentPid()) + "-" + std::to_string(sequence.fetch_add(1)) + "-" +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
#ifdef _WIN32
    endpoint_ = LR"(\\.\pipe\foundry-ipc-test-)" + fs::path(suffix).native();
#else
    directory_ = test::MakePrivateIpcDirectory();
    endpoint_ = directory_ / "s";
#endif
  }

  void TearDown() override {
    if (!directory_.empty()) {
      std::error_code error;
      fs::remove_all(directory_, error);
    }
  }

  struct Pair {
    Channel client;
    Channel server;
    PeerIdentity accepted_peer;
  };

  Pair MakePair() {
    Listener listener(endpoint_);
    auto client = Connect(endpoint_);
    auto [server, peer] = listener.Accept();
    return {std::move(client), std::move(server), peer};
  }

  template <typename T>
  bool Ready(std::future<T>& result, Pair& channels) {
    const auto ready = result.wait_for(5s) == std::future_status::ready;
    if (!ready) {
      channels.client.Close();
      channels.server.Close();
    }

    return ready;
  }

  fs::path endpoint_;
  fs::path directory_;
};

TEST_F(IpcChannelTest, AbsentEndpointIsDistinctFromInvalidEndpointAndDoesNotCreateAListener) {
  EXPECT_FALSE(TryConnect(endpoint_));
  EXPECT_FALSE(TryConnect(endpoint_));
  EXPECT_THROW(static_cast<void>(TryConnect("not-an-absolute-endpoint")), Exception);
}

Frame SampleFrame(std::uint64_t id, FrameKind kind, std::size_t size) {
  Frame frame{.invocation_id = id, .kind = kind, .payload = std::vector<std::byte>(size)};
  for (std::size_t i = 0; i < size; ++i) {
    frame.payload[i] = static_cast<std::byte>(i % 251);
  }

  return frame;
}

void ExpectFrame(const Frame& actual, const Frame& expected) {
  EXPECT_EQ(actual.invocation_id, expected.invocation_id);
  EXPECT_EQ(actual.kind, expected.kind);
  EXPECT_EQ(actual.payload, expected.payload);
}

std::optional<flErrorCode> AcceptError(Listener& listener) {
  try {
    static_cast<void>(listener.Accept());
    return std::nullopt;
  } catch (const Exception& error) {
    return error.code();
  }
}

TEST_F(IpcChannelTest, DuplexExchangePreservesInvocationKindAndOwnedPayload) {
  auto pair = MakePair();
  const auto request = SampleFrame(0x8877665544332211ULL, FrameKind::Data, kMaxFramePayload);
  const auto response = SampleFrame(0x1122334455667788ULL, FrameKind::Terminal, kMaxFramePayload);
  auto server = std::async(std::launch::async, [&] {
    const auto received = pair.server.ReadFrame();
    pair.server.WriteFrame(response);
    return received;
  });
  auto client = std::async(std::launch::async, [&] {
    pair.client.WriteFrame(request);
    return pair.client.ReadFrame();
  });

  EXPECT_TRUE(Ready(server, pair));
  EXPECT_TRUE(Ready(client, pair));
  ExpectFrame(server.get(), request);
  ExpectFrame(client.get(), response);
}

TEST_F(IpcChannelTest, EmptyPayloadAndZeroInvocationAreValid) {
  auto pair = MakePair();
  const Frame expected{};
  pair.client.WriteFrame(expected);
  ExpectFrame(pair.server.ReadFrame(), expected);
}

TEST_F(IpcChannelTest, PeerIdentityContainsActualProcessIdOnBothEnds) {
  auto pair = MakePair();
  EXPECT_EQ(pair.accepted_peer.process_id, CurrentPid());
  EXPECT_EQ(pair.server.Peer(), pair.accepted_peer);
  EXPECT_EQ(pair.client.Peer().process_id, CurrentPid());
  EXPECT_EQ(pair.server.Peer().generation, pair.client.Peer().generation);
  if (pair.accepted_peer.generation) {
    EXPECT_EQ(*pair.accepted_peer.generation, CurrentProcessIdentity().generation);
  }
#ifdef _WIN32
  FILETIME created{}, exited{}, kernel{}, user{};
  ASSERT_TRUE(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user));
  const auto expected = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  EXPECT_EQ(pair.accepted_peer.generation, expected);
#endif
}

#ifndef _WIN32
TEST_F(IpcChannelTest, SaturatedListenerDoesNotBlockConnectionAttemptsPastTheirDeadline) {
  Listener listener(endpoint_);
  std::vector<Channel> queued;
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  bool saturated = false;
  while (std::chrono::steady_clock::now() < deadline) {
    auto channel = TryConnect(endpoint_, deadline);
    if (!channel) {
      saturated = true;
      break;
    }

    queued.push_back(std::move(*channel));
  }

  ASSERT_TRUE(saturated);
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(TryConnect(endpoint_, start + 20ms));
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
}
#endif

TEST_F(IpcChannelTest, ConcurrentWritersDoNotInterleaveFrames) {
  auto pair = MakePair();
  constexpr std::size_t count = 32;
  auto reader = std::async(std::launch::async, [&] {
    std::set<std::uint64_t> ids;
    for (std::size_t i = 0; i < 2 * count; ++i) {
      const auto frame = pair.server.ReadFrame();
      ExpectFrame(frame, SampleFrame(frame.invocation_id, FrameKind::Data, 4096));
      ids.insert(frame.invocation_id);
    }

    return ids;
  });
  const auto write = [&](std::uint64_t base) {
    for (std::size_t i = 0; i < count; ++i) {
      pair.client.WriteFrame(SampleFrame(base + i, FrameKind::Data, 4096));
    }
  };
  auto first = std::async(std::launch::async, write, 0);
  auto second = std::async(std::launch::async, write, count);

  EXPECT_TRUE(Ready(reader, pair));
  EXPECT_TRUE(Ready(first, pair));
  EXPECT_TRUE(Ready(second, pair));
  first.get();
  second.get();
  const auto ids = reader.get();
  ASSERT_EQ(ids.size(), 2 * count);
  EXPECT_EQ(*ids.begin(), 0);
  EXPECT_EQ(*ids.rbegin(), 2 * count - 1);
}

TEST_F(IpcChannelTest, InvalidOutgoingFrameDoesNotPoisonConnection) {
  auto pair = MakePair();
  EXPECT_THROW(pair.client.WriteFrame(SampleFrame(1, FrameKind::Data, kMaxFramePayload + 1)), Exception);
  EXPECT_THROW(pair.client.WriteFrame(SampleFrame(1, static_cast<FrameKind>(99), 0)), Exception);

  const auto valid = SampleFrame(2, FrameKind::Control, 3);
  pair.client.WriteFrame(valid);
  ExpectFrame(pair.server.ReadFrame(), valid);
}

TEST_F(IpcChannelTest, CloseInterruptsBlockedReadAndIsIdempotent) {
  auto pair = MakePair();
  std::promise<void> started;
  auto reader = std::async(std::launch::async, [&] {
    started.set_value();
    try {
      static_cast<void>(pair.client.ReadFrame());
      return false;
    } catch (const Exception&) {
      return true;
    }
  });
  started.get_future().wait();
  EXPECT_EQ(reader.wait_for(50ms), std::future_status::timeout);

  pair.client.Close();
  pair.client.Close();
  EXPECT_TRUE(Ready(reader, pair));
  EXPECT_TRUE(reader.get());
  EXPECT_THROW(static_cast<void>(pair.server.ReadFrame()), Exception);
  EXPECT_THROW(pair.client.WriteFrame(Frame{}), Exception);
}

TEST_F(IpcChannelTest, CloseInterruptsBackpressuredWriteWithoutPeerReading) {
  auto pair = MakePair();
  const auto frame = SampleFrame(1, FrameKind::Data, kMaxFramePayload);
  std::promise<void> started;
  auto writer = std::async(std::launch::async, [&] {
    started.set_value();
    try {
      for (int i = 0; i < 10000; ++i) {
        pair.client.WriteFrame(frame);
      }

      return false;
    } catch (const Exception&) {
      return true;
    }
  });
  started.get_future().wait();
  EXPECT_EQ(writer.wait_for(100ms), std::future_status::timeout);

  pair.client.Close();
  EXPECT_TRUE(Ready(writer, pair));
  EXPECT_TRUE(writer.get());
}

TEST_F(IpcChannelTest, MoveTransfersOwnershipAndMovedFromChannelRejectsIo) {
  auto pair = MakePair();
  auto moved = std::move(pair.client);
  EXPECT_THROW(static_cast<void>(pair.client.ReadFrame()), Exception);
  pair.client.Close();
  const auto expected = SampleFrame(7, FrameKind::Control, 8);
  moved.WriteFrame(expected);
  ExpectFrame(pair.server.ReadFrame(), expected);

  pair.client = std::move(moved);
  pair.client.Close();
  EXPECT_THROW(static_cast<void>(pair.server.ReadFrame()), Exception);
}

TEST_F(IpcChannelTest, CloseInterruptsBlockedAccept) {
  Listener listener(endpoint_);
  std::promise<void> started;
  auto accept = std::async(std::launch::async, [&] {
    started.set_value();
    return AcceptError(listener);
  });
  started.get_future().wait();
  EXPECT_EQ(accept.wait_for(50ms), std::future_status::timeout);

  listener.Close();
  listener.Close();
  EXPECT_EQ(accept.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(accept.get(), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  EXPECT_EQ(AcceptError(listener), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
  EXPECT_NO_THROW(Listener replacement(endpoint_));
}

TEST_F(IpcChannelTest, ListenerCloseBeforeAcceptIsIdempotentAndRejectsFutureAccept) {
  Listener listener(endpoint_);
  listener.Close();
  listener.Close();
  EXPECT_EQ(AcceptError(listener), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);

  Listener replacement(endpoint_);
  listener.Close();
  auto client = Connect(endpoint_);
  auto [server, peer] = replacement.Accept();
  EXPECT_EQ(peer.process_id, CurrentPid());
  client.WriteFrame(Frame{});
  ExpectFrame(server.ReadFrame(), Frame{});
}

TEST_F(IpcChannelTest, ListenerClosePreservesAcceptedChannels) {
  Listener listener(endpoint_);
  auto client = Connect(endpoint_);
  auto [server, peer] = listener.Accept();
  listener.Close();
  EXPECT_EQ(AcceptError(listener), FOUNDRY_LOCAL_ERROR_INVALID_USAGE);

  const auto expected = SampleFrame(19, FrameKind::Control, 8);
  client.WriteFrame(expected);
  ExpectFrame(server.ReadFrame(), expected);
}

TEST_F(IpcChannelTest, ExistingListenerCannotBeTakenOverAndNameIsReusableAfterDestruction) {
  {
    Listener first(endpoint_);
    EXPECT_THROW(Listener second(endpoint_), Exception);
    auto client = Connect(endpoint_);
    auto [server, peer] = first.Accept();
    EXPECT_EQ(peer.process_id, CurrentPid());
    client.WriteFrame(Frame{});
    ExpectFrame(server.ReadFrame(), Frame{});
  }

#ifndef _WIN32
  EXPECT_FALSE(fs::exists(endpoint_));
#endif
  EXPECT_NO_THROW(Listener next(endpoint_));
}

class RawPeer {
 public:
  explicit RawPeer(const fs::path& endpoint) {
#ifdef _WIN32
    handle_ = CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                          SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      throw std::runtime_error("Raw pipe connect failed");
    }
#else
#ifdef __linux__
    fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
#endif
    if (fd_ < 0) {
      throw std::runtime_error("Raw socket failed");
    }

#ifdef __APPLE__
    const int enabled = 1;
    if (fcntl(fd_, F_SETFD, FD_CLOEXEC) < 0 ||
        setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
      Close();
      throw std::runtime_error("Raw socket flags failed");
    }
#endif
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto text = endpoint.native();
    if (text.size() >= sizeof(address.sun_path)) {
      Close();
      throw std::runtime_error("Raw socket path too long");
    }

    std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
    if (connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
      Close();
      throw std::runtime_error("Raw socket connect failed");
    }
#endif
  }

  ~RawPeer() { Close(); }
  RawPeer(const RawPeer&) = delete;
  RawPeer& operator=(const RawPeer&) = delete;

  void Close() noexcept {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(std::exchange(handle_, INVALID_HANDLE_VALUE));
    }
#else
    if (fd_ >= 0) {
      close(std::exchange(fd_, -1));
    }
#endif
  }

  void Write(std::span<const std::byte> bytes, std::size_t fragment_size = 7) {
    while (!bytes.empty()) {
      const auto size = std::min(bytes.size(), fragment_size);
#ifdef _WIN32
      DWORD written = 0;
      if (!WriteFile(handle_, bytes.data(), static_cast<DWORD>(size), &written, nullptr) || written == 0) {
        throw std::runtime_error("Raw pipe write failed");
      }
#else
#ifdef __linux__
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      const auto written = send(fd_, bytes.data(), size, flags);
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written <= 0) {
        throw std::runtime_error("Raw socket write failed");
      }
#endif
      bytes = bytes.subspan(static_cast<std::size_t>(written));
    }
  }

 private:
#ifdef _WIN32
  HANDLE handle_{INVALID_HANDLE_VALUE};
#else
  int fd_{-1};
#endif
};

std::array<std::byte, 20> WireHeader(std::size_t length) {
  std::array<std::byte, 20> header{std::byte{'F'}, std::byte{'L'}, std::byte{'I'},
                                   std::byte{'P'}, std::byte{1}, std::byte{2}};
  for (std::size_t i = 0; i < 4; ++i) {
    header[8 + i] = static_cast<std::byte>((length >> (8 * i)) & 0xff);
  }

  header[12] = std::byte{42};
  return header;
}

#ifdef _WIN32
TEST_F(IpcChannelTest, ClientDisconnectBeforeAcceptDoesNotCloseTheListener) {
  Listener listener(endpoint_);
  {
    RawPeer abandoned(endpoint_);
  }

  auto accepted = std::async(std::launch::async, [&] { return listener.Accept(); });
  ScopeGuard close([&]() noexcept { listener.Close(); });
  std::optional<Channel> client;
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  do {
    client = TryConnect(endpoint_);
    if (!client) {
      std::this_thread::sleep_for(5ms);
    }
  } while (!client && std::chrono::steady_clock::now() < deadline);

  ASSERT_TRUE(client);
  ASSERT_EQ(accepted.wait_for(5s), std::future_status::ready);
  auto [server, peer] = accepted.get();
  EXPECT_EQ(peer.process_id, CurrentPid());
  const auto frame = SampleFrame(7, FrameKind::Data, 8);
  client->WriteFrame(frame);
  ExpectFrame(server.ReadFrame(), frame);
}
#endif

#ifndef _WIN32
TEST_F(IpcChannelTest, DescriptorExhaustionRecoversAndRemainsInterruptibleByClose) {
  const auto style = GTEST_FLAG_GET(death_test_style);
  ScopeGuard restore_style([&]() noexcept { GTEST_FLAG_SET(death_test_style, style); });
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  for (const bool close_listener : {false, true}) {
    ASSERT_EXIT(
        {
          const auto exercise = [&]() -> bool {
            Listener listener(endpoint_);
            RawPeer pending(endpoint_);
            rlimit original{};
            if (getrlimit(RLIMIT_NOFILE, &original) != 0) {
              return false;
            }

            auto limited = original;
            limited.rlim_cur = std::min<rlim_t>(original.rlim_cur, 128);
            if (setrlimit(RLIMIT_NOFILE, &limited) != 0) {
              return false;
            }

            std::vector<int> descriptors;
            const auto restore = [&]() noexcept {
              for (const auto fd : descriptors) {
                close(fd);
              }

              descriptors.clear();
              setrlimit(RLIMIT_NOFILE, &original);
            };
            ScopeGuard restore_limit(restore);
            for (int fd; (fd = open("/dev/null", O_RDONLY)) >= 0;) {
              descriptors.push_back(fd);
            }

            auto accepted = std::async(std::launch::async, [&] { return listener.Accept(); });
            ScopeGuard close_accept([&]() noexcept { listener.Close(); });
            const auto blocked = accepted.wait_for(50ms) == std::future_status::timeout;
            if (close_listener) {
              listener.Close();
            } else {
              restore();
            }

            if (accepted.wait_for(5s) != std::future_status::ready) {
              _exit(1);
            }
            if (close_listener) {
              try {
                static_cast<void>(accepted.get());
              } catch (const Exception& error) {
                return blocked && error.code() == FOUNDRY_LOCAL_ERROR_INVALID_USAGE;
              }

              return false;
            }

            auto first = accepted.get();
            first.first.Close();
            RawPeer healthy(endpoint_);
            auto next = listener.Accept();
            const auto frame = SampleFrame(7, FrameKind::Data, 8);
            healthy.Write(WireHeader(frame.payload.size()), 20);
            healthy.Write(frame.payload, frame.payload.size());
            const auto received = next.first.ReadFrame();
            return blocked && next.second.process_id == CurrentPid() && received.payload == frame.payload;
          };
          try {
            const auto success = exercise();
            TearDown();
            _exit(success ? 0 : 1);
          } catch (...) {
            _exit(1);
          }
        },
        testing::ExitedWithCode(0), "");
  }
}
#endif

TEST_F(IpcChannelTest, FragmentedHeaderAndMaximumPayloadAreReassembledExactly) {
  Listener listener(endpoint_);
  RawPeer raw(endpoint_);
  auto [channel, peer] = listener.Accept();
  const auto expected = SampleFrame(42, FrameKind::Data, kMaxFramePayload);
  auto reader = std::async(std::launch::async, [&] { return channel.ReadFrame(); });
  raw.Write(WireHeader(expected.payload.size()), 1);
  raw.Write(expected.payload, 127);

  if (reader.wait_for(5s) != std::future_status::ready) {
    channel.Close();
    ADD_FAILURE() << "Fragmented frame read did not complete";
  }

  ExpectFrame(reader.get(), expected);
}

TEST_F(IpcChannelTest, MalformedAndTruncatedFramesAreRejectedAndChannelIsClosed) {
  std::vector<std::vector<std::byte>> inputs;
  for (const auto offset : {0, 4, 5, 6, 7}) {
    auto header = WireHeader(0);
    header[static_cast<std::size_t>(offset)] = std::byte{0xff};
    inputs.emplace_back(header.begin(), header.end());
  }

  const auto oversized = WireHeader(kMaxFramePayload + 1);
  inputs.emplace_back(oversized.begin(), oversized.end());
  const auto valid = WireHeader(5);
  inputs.emplace_back(valid.begin(), valid.begin() + 3);
  inputs.emplace_back(valid.begin(), valid.end());
  inputs.back().insert(inputs.back().end(), 2, std::byte{9});
  inputs.emplace_back();

  for (const auto& bytes : inputs) {
    Listener listener(endpoint_);
    RawPeer raw(endpoint_);
    auto [channel, peer] = listener.Accept();
    raw.Write(bytes, bytes.empty() ? 1 : bytes.size());
    raw.Close();
    EXPECT_THROW(static_cast<void>(channel.ReadFrame()), Exception);
    EXPECT_THROW(channel.WriteFrame(Frame{}), Exception);
  }
}

#ifndef _WIN32
TEST_F(IpcChannelTest, SocketIsOwnerOnlyAndEndpointDirectoryIsNotRemoved) {
  {
    Listener listener(endpoint_);
    struct stat status{};
    ASSERT_EQ(lstat(endpoint_.c_str(), &status), 0);
    EXPECT_TRUE(S_ISSOCK(status.st_mode));
    EXPECT_EQ(status.st_uid, geteuid());
    EXPECT_EQ(status.st_mode & 0777, 0600);
  }

  EXPECT_FALSE(fs::exists(endpoint_));
  EXPECT_TRUE(fs::is_directory(directory_));
}

TEST_F(IpcChannelTest, PreexistingFileIsNotRemovedOrOverwritten) {
  {
    std::ofstream file(endpoint_);
    file << "preserve";
  }

  EXPECT_THROW(Listener listener(endpoint_), Exception);
  std::ifstream file(endpoint_);
  std::string contents;
  file >> contents;
  EXPECT_EQ(contents, "preserve");
  EXPECT_THROW(static_cast<void>(Connect(endpoint_)), Exception);
}

TEST_F(IpcChannelTest, ListenerDoesNotUnlinkReplacementEndpoint) {
  {
    Listener listener(endpoint_);
    ASSERT_TRUE(fs::remove(endpoint_));
    std::ofstream replacement(endpoint_);
    replacement << "replacement";
  }

  std::ifstream file(endpoint_);
  std::string contents;
  file >> contents;
  EXPECT_EQ(contents, "replacement");
}

TEST_F(IpcChannelTest, SharedDirectoryAndSymlinkDirectoryAreRejected) {
  fs::permissions(directory_, fs::perms::group_read, fs::perm_options::add);
  EXPECT_THROW(Listener listener(endpoint_), Exception);
  fs::permissions(directory_, fs::perms::owner_all, fs::perm_options::replace);
  fs::create_directory_symlink(directory_, directory_ / "alias");
  EXPECT_THROW(Listener listener(directory_ / "alias" / "s"), Exception);
}

#ifdef __APPLE__
TEST_F(IpcChannelTest, ExtendedDirectoryAclIsRejectedEvenWithMode0700) {
  struct Acl {
    acl_t value{acl_init(1)};
    ~Acl() { acl_free(value); }
  } acl;
  ASSERT_NE(acl.value, nullptr);
  acl_entry_t entry{};
  ASSERT_EQ(acl_create_entry(&acl.value, &entry), 0);
  ASSERT_EQ(acl_set_tag_type(entry, ACL_EXTENDED_ALLOW), 0);
  uuid_t user{};
  ASSERT_EQ(mbr_uid_to_uuid(geteuid(), user), 0);
  ASSERT_EQ(acl_set_qualifier(entry, user), 0);
  acl_permset_t permissions{};
  ASSERT_EQ(acl_get_permset(entry, &permissions), 0);
  ASSERT_EQ(acl_add_perm(permissions, ACL_READ_DATA), 0);
  ASSERT_EQ(acl_set_file(directory_.c_str(), ACL_TYPE_EXTENDED, acl.value), 0);
  EXPECT_THROW(Listener listener(endpoint_), Exception);
}
#endif
#else
TEST_F(IpcChannelTest, RemotePipeAndNestedPipeNamesAreRejected) {
  EXPECT_THROW(Listener listener(LR"(\\remote\pipe\foundry-test)"), Exception);
  EXPECT_THROW(static_cast<void>(Connect(LR"(\\remote\pipe\foundry-test)")), Exception);
  EXPECT_THROW(Listener listener(LR"(\\.\pipe\nested\foundry-test)"), Exception);
}
#endif

}  // namespace
}  // namespace fl::hosting
