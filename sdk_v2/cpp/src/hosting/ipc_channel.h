// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace fl::hosting {

inline constexpr std::size_t kMaxFramePayload = 64 * 1024;

enum class FrameKind : std::uint8_t { Control = 1,
                                      Data = 2,
                                      Terminal = 3 };

struct Frame {
  std::uint64_t invocation_id{};
  FrameKind kind{FrameKind::Control};
  std::vector<std::byte> payload;
};

struct PeerIdentity {
  std::uint64_t process_id{};
  // OS-specific creation identity, not a timestamp shared between platforms. Absence is not proof of liveness.
  std::optional<std::uint64_t> generation;

  bool operator==(const PeerIdentity&) const = default;
};

class Listener;

class Channel {
 public:
  Channel() noexcept = default;
  ~Channel();
  Channel(Channel&& other) noexcept;
  Channel& operator=(Channel&& other) noexcept;
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;

  // One reader and one writer may run concurrently. Multiple readers/writers serialize whole frames.
  // Malformed input, EOF (including at a frame boundary), and I/O failure throw and close the channel.
  [[nodiscard]] Frame ReadFrame();
  void WriteFrame(const Frame& frame);
  [[nodiscard]] PeerIdentity Peer() const;

  // May race ReadFrame/WriteFrame/Close, but not move, destruction, or replacement of this Channel object.
  void Close() noexcept;

 private:
  struct State;
  explicit Channel(std::shared_ptr<State> state) noexcept;
  [[nodiscard]] std::shared_ptr<State> RequireState() const;
  static std::mutex& ReadMutex(State& state);
  static std::mutex& WriteMutex(State& state);
  static void ReadExact(State& state, std::span<std::byte> bytes);
  static void WriteExact(State& state, std::span<const std::byte> bytes);

  std::shared_ptr<State> state_;
  friend class Listener;
  friend Channel Connect(const std::filesystem::path& endpoint);
  friend std::optional<Channel> TryConnect(const std::filesystem::path& endpoint);
};

// Windows endpoints are full local \\.\pipe\ names. Unix endpoints are absolute paths in an existing owner-only
// directory. Listener destruction/move must not race Accept or Close. No stale recovery or process monitoring.
class Listener {
 public:
  explicit Listener(const std::filesystem::path& endpoint);
  ~Listener();
  Listener(Listener&& other) noexcept;
  Listener& operator=(Listener&& other) noexcept;
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  [[nodiscard]] std::pair<Channel, PeerIdentity> Accept();

  // Idempotent; may race Accept/Close, but not move/destruction. Join accept workers before destroying the listener.
  // Releases the listener endpoint as active I/O unwinds, not proof that the hosting process has exited.
  void Close() noexcept;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

// Connect performs one connection attempt; endpoint discovery, startup deadlines and retry policy belong to the caller.
[[nodiscard]] Channel Connect(const std::filesystem::path& endpoint);
// Null means absent/not listening or temporarily busy, never a permission, identity or protocol failure.
[[nodiscard]] std::optional<Channel> TryConnect(const std::filesystem::path& endpoint);

}  // namespace fl::hosting
