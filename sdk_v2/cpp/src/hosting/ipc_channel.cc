// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/ipc_channel.h"

#include "exception.h"

#include <algorithm>
#include <array>

namespace fl::hosting {
namespace {

// Fixed little-endian wire fields: magic[4], version[1], kind[1], reserved[2], length[4], invocation[8].
constexpr std::array<std::byte, 4> kMagic{std::byte{'F'}, std::byte{'L'}, std::byte{'I'}, std::byte{'P'}};
constexpr std::size_t kHeaderSize = 20;
using Header = std::array<std::byte, kHeaderSize>;

bool ValidKind(FrameKind kind) {
  return kind == FrameKind::Control || kind == FrameKind::Data || kind == FrameKind::Terminal;
}

void Encode(std::span<std::byte> bytes, std::uint64_t value) {
  for (auto& byte : bytes) {
    byte = static_cast<std::byte>(value & 0xff);
    value >>= 8;
  }
}

std::uint64_t Decode(std::span<const std::byte> bytes) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
  }

  return value;
}

Header EncodeHeader(const Frame& frame) {
  Header header{};
  std::copy(kMagic.begin(), kMagic.end(), header.begin());
  header[4] = std::byte{1};
  header[5] = static_cast<std::byte>(frame.kind);
  Encode(std::span{header}.subspan(8, 4), frame.payload.size());
  Encode(std::span{header}.subspan(12, 8), frame.invocation_id);
  return header;
}

Frame DecodeHeader(const Header& header) {
  const auto kind = static_cast<FrameKind>(header[5]);
  const auto length = Decode(std::span{header}.subspan(8, 4));
  if (!std::equal(kMagic.begin(), kMagic.end(), header.begin()) || header[4] != std::byte{1} ||
      header[6] != std::byte{} || header[7] != std::byte{} || !ValidKind(kind) || length > kMaxFramePayload) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Invalid native IPC frame header");
  }

  return Frame{.invocation_id = Decode(std::span{header}.subspan(12, 8)),
               .kind = kind,
               .payload = std::vector<std::byte>(static_cast<std::size_t>(length))};
}

}  // namespace

Channel::Channel(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

Channel::~Channel() { Close(); }

Channel::Channel(Channel&& other) noexcept : state_(std::move(other.state_)) {}

Channel& Channel::operator=(Channel&& other) noexcept {
  if (this != &other) {
    Close();
    state_ = std::move(other.state_);
  }

  return *this;
}

std::shared_ptr<Channel::State> Channel::RequireState() const {
  if (!state_) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Native IPC channel is empty");
  }

  return state_;
}

Frame Channel::ReadFrame() {
  const auto state = RequireState();
  const std::lock_guard lock(ReadMutex(*state));
  try {
    Header header{};
    ReadExact(*state, header);
    auto frame = DecodeHeader(header);
    ReadExact(*state, frame.payload);
    return frame;
  } catch (...) {
    Close();
    throw;
  }
}

void Channel::WriteFrame(const Frame& frame) {
  if (!ValidKind(frame.kind) || frame.payload.size() > kMaxFramePayload) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "Native IPC frame kind or payload size is invalid");
  }

  const auto state = RequireState();
  const std::lock_guard lock(WriteMutex(*state));
  try {
    const auto header = EncodeHeader(frame);
    WriteExact(*state, header);
    WriteExact(*state, frame.payload);
  } catch (...) {
    Close();
    throw;
  }
}

}  // namespace fl::hosting
