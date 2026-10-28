// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/sdk_connection.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fl::hosting {
namespace {

using Json = nlohmann::json;
constexpr std::size_t kHeaderBytes = 20;
constexpr std::size_t kControlQueueBytes = 64 * 1024;
enum class MessageKind : std::uint8_t { Request = 1,
                                        Event,
                                        Result,
                                        Input,
                                        Finish,
                                        Cancel,
                                        Pull,
                                        Ready,
                                        InputClosed };

bool IsControl(MessageKind kind) {
  return kind == MessageKind::Cancel || kind == MessageKind::Pull || kind == MessageKind::Ready ||
         kind == MessageKind::InputClosed;
}

class InvocationCancelled : public std::runtime_error {
 public:
  InvocationCancelled() : std::runtime_error("SDK invocation cancelled or completed") {}
};

struct Message {
  Json value;
  std::int32_t status{};
  std::string error;
};

void to_json(Json& json, const Message& message) {
  json = Json{{"value", message.value}, {"status", message.status}, {"error", message.error}};
}

void from_json(const Json& json, Message& message) {
  if (!json.is_object() || !json.contains("value") || !json.contains("status") || !json.contains("error") ||
      !json.at("status").is_number_integer() || !json.at("error").is_string()) {
    throw std::runtime_error("Invalid SDK message: required value/status/error fields");
  }

  const auto& status = json.at("status");
  if ((status.is_number_unsigned() && status.get<std::uint64_t>() > INT32_MAX) ||
      (!status.is_number_unsigned() &&
       (status.get<std::int64_t>() < INT32_MIN || status.get<std::int64_t>() > INT32_MAX))) {
    throw std::runtime_error("Invalid SDK message: status outside int32 range");
  }

  message = {json.at("value"), status.get<std::int32_t>(), json.at("error").get<std::string>()};
}

bool ValidOperation(Operation op) { return op >= Operation::Handshake && op <= Operation::RuntimeDevices; }

void Encode(std::span<std::byte> bytes, std::uint64_t value) {
  for (auto& byte : bytes) {
    byte = static_cast<std::byte>(value & 0xff);
    value >>= 8;
  }
}

std::uint64_t Decode(std::span<const std::byte> bytes) {
  std::uint64_t value{};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
  }

  return value;
}

struct Fragment {
  MessageKind kind{};
  Operation op{};
  std::uint64_t total{};
  std::uint64_t offset{};
};

Fragment Parse(const Frame& frame) {
  if (!frame.invocation_id || frame.payload.size() < kHeaderBytes ||
      frame.payload[0] != static_cast<std::byte>(SdkConnection::kProtocolVersion)) {
    throw std::runtime_error("Invalid SDK fragment identity/version/header");
  }

  const std::span bytes{frame.payload};
  Fragment fragment{static_cast<MessageKind>(bytes[1]), static_cast<Operation>(Decode(bytes.subspan(2, 2))),
                    Decode(bytes.subspan(4, 8)), Decode(bytes.subspan(12, 8))};
  const auto size = bytes.size() - kHeaderBytes;
  const bool control = IsControl(fragment.kind);
  if (fragment.kind < MessageKind::Request || fragment.kind > MessageKind::InputClosed ||
      (fragment.kind == MessageKind::Request ? !ValidOperation(fragment.op) : fragment.op != Operation{}) ||
      fragment.total > std::numeric_limits<std::size_t>::max() || fragment.offset > fragment.total ||
      size > fragment.total - fragment.offset ||
      (control ? (fragment.total != 0 || size != 0)
               : (fragment.total == 0 || size == 0 || fragment.offset == fragment.total)) ||
      frame.kind != (control ? FrameKind::Control : FrameKind::Data)) {
    throw std::runtime_error("Invalid SDK fragment kind/length/operation");
  }

  return fragment;
}

Frame MakeFragment(std::uint64_t id, MessageKind kind, Operation op, std::uint64_t total, std::uint64_t offset,
                   std::span<const std::uint8_t> data = {}) {
  Frame frame{.invocation_id = id,
              .kind = IsControl(kind) ? FrameKind::Control : FrameKind::Data,
              .payload = std::vector<std::byte>(kHeaderBytes + data.size())};
  std::span bytes{frame.payload};
  bytes[0] = static_cast<std::byte>(SdkConnection::kProtocolVersion);
  bytes[1] = static_cast<std::byte>(kind);
  Encode(bytes.subspan(2, 2), static_cast<std::uint16_t>(op));
  Encode(bytes.subspan(4, 8), total);
  Encode(bytes.subspan(12, 8), offset);
  std::transform(data.begin(), data.end(), bytes.begin() + kHeaderBytes,
                 [](std::uint8_t byte) { return static_cast<std::byte>(byte); });
  return frame;
}

class Assembly {
 public:
  std::optional<Message> Add(const Frame& frame) {
    const auto fragment = Parse(frame);
    if (bytes_.empty()) {
      first_ = fragment;
    }

    if (fragment.kind != first_.kind || fragment.op != first_.op || fragment.total != first_.total ||
        fragment.offset != bytes_.size()) {
      throw std::runtime_error("Invalid SDK fragment sequence");
    }

    for (auto it = frame.payload.begin() + kHeaderBytes; it != frame.payload.end(); ++it) {
      bytes_.push_back(static_cast<std::uint8_t>(*it));
    }

    if (bytes_.size() != fragment.total) {
      return std::nullopt;
    }

    auto result = Json::from_cbor(bytes_).get<Message>();
    std::vector<std::uint8_t>{}.swap(bytes_);
    return result;
  }

 private:
  Fragment first_{};
  std::vector<std::uint8_t> bytes_;
};

}  // namespace

struct SdkConnection::Invocation {
  InvocationId id{};
  Operation op{};
  Json payload;
  Assembly request;
  std::mutex send_mutex;
  std::mutex input_mutex;
  bool started{};
  bool request_written{};
  bool complete{};
  bool input_finished{};
  bool peer_input_closed{};
  bool cancelled{};
  bool cancel_notified{};
  bool peer_permit{};
  bool offer_pending{};
  bool ready{};
  bool receive_requested{};
  std::optional<Frame> delivery;
  std::function<void()> cancel_handler;
};

struct SdkConnection::State {
  explicit State(Channel channel_value, Side side_value) : channel(std::move(channel_value)), side(side_value) {}

  Channel channel;
  const Side side;
  std::mutex mutex;
  std::condition_variable changed;
  std::exception_ptr failure;
  bool stopping{};
  bool disconnect_notified{};
  std::function<void()> disconnect_handler;
  std::deque<std::function<void()>> cancel_hooks;
  InvocationId next_id{1};
  std::unordered_map<InvocationId, std::shared_ptr<Invocation>> calls;
  std::unordered_set<InvocationId> seen;
  std::unordered_set<InvocationId> early_cancels;
  std::deque<std::shared_ptr<Invocation>> incoming;
  std::deque<Frame> data;
  std::deque<Frame> controls;
  std::size_t data_bytes{};
  std::size_t control_bytes{};
  std::size_t receive_bytes{};

  void Check() const {
    if (failure) {
      std::rethrow_exception(failure);
    }
  }

  void QueueCancelHook(Invocation& call) {
    if (call.cancelled && !call.cancel_notified && call.cancel_handler) {
      call.cancel_notified = true;
      cancel_hooks.push_back(call.cancel_handler);
    }
  }

  void Fail(std::exception_ptr error) noexcept {
    {
      const std::lock_guard lock(mutex);
      if (!failure) {
        failure = error;
        for (auto& [id, call] : calls) {
          static_cast<void>(id);
          call->cancelled = true;
          QueueCancelHook(*call);
        }
      }
    }

    channel.Close();
    changed.notify_all();
  }

  std::shared_ptr<Invocation> Find(InvocationId id) {
    const auto it = calls.find(id);
    if (it == calls.end()) {
      throw std::runtime_error("Unknown SDK invocation");
    }

    return it->second;
  }

  std::shared_ptr<Invocation> InputInvocation(InvocationId id) {
    std::unique_lock lock(mutex);
    Check();
    if (side != Side::Client) {
      throw std::runtime_error("SDK input requires client side");
    }

    if (seen.contains(id)) {
      throw SdkInputClosed();
    }

    auto call = Find(id);
    changed.wait(lock, [&] {
      return failure || call->request_written || call->cancelled || call->complete || call->peer_input_closed;
    });
    Check();
    if (call->cancelled || call->complete || call->peer_input_closed) {
      throw SdkInputClosed();
    }

    return call;
  }

  void Retire(const std::shared_ptr<Invocation>& call) {
    const std::lock_guard lock(mutex);
    if (call->receive_requested) {
      receive_bytes -= kMaxFramePayload;
      call->receive_requested = false;
      call->delivery.reset();
    }

    seen.insert(call->id);
    calls.erase(call->id);
    changed.notify_all();
  }

  void Queue(Frame frame) {
    const bool control = frame.kind == FrameKind::Control;
    const auto size = frame.payload.size();
    std::unique_lock lock(mutex);
    auto& count = control ? control_bytes : data_bytes;
    changed.wait(lock,
                 [&] { return failure || size <= (control ? kControlQueueBytes : kTransportQueueBytes) - count; });
    Check();
    count += size;
    (control ? controls : data).push_back(std::move(frame));
    changed.notify_all();
  }

  void Send(const std::shared_ptr<Invocation>& call, MessageKind kind, const Message& message) {
    const auto bytes = Json::to_cbor(Json(message));
    for (std::size_t offset = 0; offset < bytes.size();) {
      if (kind != MessageKind::Request) {
        bool offer{};
        {
          const std::lock_guard lock(mutex);
          offer = !std::exchange(call->offer_pending, true);
        }

        if (offer) {
          Queue(MakeFragment(call->id, MessageKind::Ready, Operation{}, 0, 0));
        }

        std::unique_lock lock(mutex);
        changed.wait(lock, [&] {
          return failure || call->peer_permit || call->peer_input_closed ||
                 ((call->cancelled || call->complete) && kind != MessageKind::Result);
        });
        Check();
        if (call->peer_input_closed) {
          throw SdkInputClosed();
        }
        if ((call->cancelled || call->complete) && kind != MessageKind::Result) {
          if (side == Side::Client) {
            throw SdkInputClosed();
          }

          throw InvocationCancelled();
        }

        call->peer_permit = false;
      }

      const auto count = std::min(kMaxFramePayload - kHeaderBytes, bytes.size() - offset);
      Queue(MakeFragment(call->id, kind, kind == MessageKind::Request ? call->op : Operation{}, bytes.size(), offset,
                         std::span{bytes}.subspan(offset, count)));
      if (kind != MessageKind::Request) {
        const std::lock_guard lock(mutex);
        call->offer_pending = false;
      }

      offset += count;
    }
  }

  Frame Receive(const std::shared_ptr<Invocation>& call) {
    {
      std::unique_lock lock(mutex);
      // Idle invocations reserve no bytes: only an actual offered frame can consume a delivery slot.
      changed.wait(lock, [&] {
        return failure || (side == Side::Server && (call->cancelled || call->complete)) ||
               (call->ready && receive_bytes <= kDeliveryQueueBytes - kMaxFramePayload);
      });
      Check();
      if (side == Side::Server && (call->cancelled || call->complete)) {
        throw InvocationCancelled();
      }

      if (call->receive_requested) {
        throw std::runtime_error("Concurrent SDK delivery consumers");
      }

      receive_bytes += kMaxFramePayload;
      call->ready = false;
      call->receive_requested = true;
    }

    Queue(MakeFragment(call->id, MessageKind::Pull, Operation{}, 0, 0));
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] {
      return failure || call->delivery.has_value() || (side == Side::Server && (call->cancelled || call->complete));
    });
    Check();
    if (side == Side::Server && (call->cancelled || call->complete)) {
      // A previously issued permit can still arrive. Keep its slot reserved until the invocation is retired.
      throw InvocationCancelled();
    }

    auto frame = std::move(*call->delivery);
    call->delivery.reset();
    call->receive_requested = false;
    receive_bytes -= kMaxFramePayload;
    changed.notify_all();
    return frame;
  }

  void ReadRequest(Frame frame, const Fragment& fragment) {
    std::shared_ptr<Invocation> call;
    {
      const std::lock_guard lock(mutex);
      if (fragment.offset == 0) {
        if (!seen.insert(frame.invocation_id).second) {
          throw std::runtime_error("Reused SDK invocation identity");
        }

        call = std::make_shared<Invocation>();
        call->id = frame.invocation_id;
        call->op = fragment.op;
        call->cancelled = early_cancels.erase(call->id) != 0;
        calls.emplace(call->id, call);
      } else {
        call = Find(frame.invocation_id);
      }
    }

    auto message = call->request.Add(frame);
    if (message) {
      if (message->status != 0 || !message->error.empty()) {
        throw std::runtime_error("Invalid SDK request status");
      }

      const std::lock_guard lock(mutex);
      call->payload = std::move(message->value);
      call->started = true;
      incoming.push_back(call);
      changed.notify_all();
    }
  }

  void ReadOne(Frame frame) {
    const auto fragment = Parse(frame);
    if (fragment.kind == MessageKind::Request) {
      if (side != Side::Server) {
        throw std::runtime_error("SDK request sent to client");
      }

      ReadRequest(std::move(frame), fragment);
      return;
    }

    const std::lock_guard lock(mutex);
    const auto it = calls.find(frame.invocation_id);
    if (it == calls.end()) {
      if (side == Side::Server && fragment.kind == MessageKind::Cancel) {
        if (!seen.contains(frame.invocation_id)) {
          early_cancels.insert(frame.invocation_id);
        }

        return;
      }

      if (seen.contains(frame.invocation_id)) {
        return;
      }
    }

    auto call = Find(frame.invocation_id);
    if (fragment.kind == MessageKind::InputClosed && side == Side::Client) {
      call->peer_input_closed = true;
    } else if (fragment.kind == MessageKind::Cancel && side == Side::Server) {
      call->cancelled = true;
      QueueCancelHook(*call);
    } else if (fragment.kind == MessageKind::Ready) {
      if (call->ready) {
        throw std::runtime_error("Duplicate SDK delivery offer");
      }

      call->ready = true;
    } else if (fragment.kind == MessageKind::Pull) {
      if (!call->offer_pending || call->peer_permit) {
        throw std::runtime_error("Duplicate SDK delivery permit");
      }

      call->peer_permit = true;
    } else {
      const bool valid = side == Side::Client
                             ? fragment.kind == MessageKind::Event || fragment.kind == MessageKind::Result
                             : fragment.kind == MessageKind::Input || fragment.kind == MessageKind::Finish;
      if (!valid || !call->receive_requested || call->delivery) {
        throw std::runtime_error("Unsolicited SDK delivery");
      }

      call->delivery = std::move(frame);
    }

    changed.notify_all();
  }

  void Reader() noexcept {
    try {
      while (true) {
        ReadOne(channel.ReadFrame());
      }
    } catch (...) {
      Fail(std::current_exception());
    }
  }

  void Writer() noexcept {
    try {
      while (true) {
        Frame frame;
        {
          std::unique_lock lock(mutex);
          changed.wait(lock, [&] { return failure || !controls.empty() || !data.empty(); });
          Check();
          auto& queue = controls.empty() ? data : controls;
          auto& count = controls.empty() ? data_bytes : control_bytes;
          frame = std::move(queue.front());
          count -= frame.payload.size();
          queue.pop_front();
          changed.notify_all();
        }

        channel.WriteFrame(frame);
        const auto fragment = Parse(frame);
        if (fragment.kind == MessageKind::Request &&
            fragment.offset + frame.payload.size() - kHeaderBytes == fragment.total) {
          const std::lock_guard lock(mutex);
          Find(frame.invocation_id)->request_written = true;
          changed.notify_all();
        }
      }
    } catch (...) {
      Fail(std::current_exception());
    }
  }

  std::function<void()> NextHook() {
    if (!cancel_hooks.empty()) {
      auto hook = std::move(cancel_hooks.front());
      cancel_hooks.pop_front();
      return hook;
    }

    if (failure && !disconnect_notified && disconnect_handler) {
      disconnect_notified = true;
      return disconnect_handler;
    }

    return {};
  }

  void ControlWorker() noexcept {
    while (true) {
      std::function<void()> hook;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] {
          hook = NextHook();
          return stopping || static_cast<bool>(hook);
        });
        if (!hook && stopping) {
          return;
        }
      }

      try {
        hook();
      } catch (...) {
        Fail(std::current_exception());
      }
    }
  }
};

struct SdkConnection::Impl {
  std::shared_ptr<State> state;
  std::thread reader;
  std::thread writer;
  std::thread control;

  explicit Impl(Channel channel, Side side) : state(std::make_shared<State>(std::move(channel), side)) {
    try {
      reader = std::thread([state = state] { state->Reader(); });
      writer = std::thread([state = state] { state->Writer(); });
      control = std::thread([state = state] { state->ControlWorker(); });
    } catch (...) {
      Stop();
      throw;
    }
  }

  void Stop() noexcept {
    state->Fail(std::make_exception_ptr(std::runtime_error("SDK connection closed")));
    {
      const std::lock_guard lock(state->mutex);
      state->stopping = true;
    }

    state->changed.notify_all();
    for (auto* thread : {&reader, &writer, &control}) {
      if (thread->joinable()) {
        if (thread->get_id() == std::this_thread::get_id()) {
          // Only the control worker invokes user code; its captured State owns no threads.
          thread->detach();
        } else {
          thread->join();
        }
      }
    }
  }

  ~Impl() { Stop(); }
};

SdkConnection::SdkConnection(Channel channel, Side side) : impl_(std::make_unique<Impl>(std::move(channel), side)) {}
SdkConnection::~SdkConnection() = default;

SdkConnection::InvocationId SdkConnection::AllocateInvocation() {
  const auto state = impl_->state;
  const std::lock_guard lock(state->mutex);
  state->Check();
  if (state->side != Side::Client || !state->next_id) {
    throw std::runtime_error("Cannot allocate SDK invocation");
  }

  auto call = std::make_shared<Invocation>();
  call->id = state->next_id++;
  state->calls.emplace(call->id, call);
  return call->id;
}

SdkResult SdkConnection::Call(Operation op, const Json& payload, EventCallback callback) {
  return Call(AllocateInvocation(), op, payload, std::move(callback));
}

SdkResult SdkConnection::Call(InvocationId id, Operation op, const Json& payload, EventCallback callback) {
  const auto state = impl_->state;
  std::shared_ptr<Invocation> call;
  {
    const std::lock_guard lock(state->mutex);
    state->Check();
    call = state->Find(id);
    if (state->side != Side::Client || call->started || !ValidOperation(op)) {
      throw std::runtime_error("Invalid SDK call operation/identity");
    }

    call->started = true;
    call->op = op;
  }

  try {
    state->Send(call, MessageKind::Request, Message{payload, 0, {}});
    Assembly assembly;
    while (true) {
      auto frame = state->Receive(call);
      const auto fragment = Parse(frame);
      if (fragment.kind == MessageKind::Result && fragment.offset == 0) {
        const std::lock_guard lock(state->mutex);
        if (call->cancelled) {
          // A cancelled sender may replace the remainder of an event; partial values never reach callbacks.
          assembly = Assembly{};
        }
      }

      auto message = assembly.Add(frame);
      if (!message) {
        continue;
      }

      if (fragment.kind == MessageKind::Result) {
        const std::lock_guard lock(state->mutex);
        call->complete = true;
        state->seen.insert(id);
        state->calls.erase(id);
        state->changed.notify_all();
        return {message->status, std::move(message->value), std::move(message->error)};
      }

      if (message->status != 0 || !message->error.empty()) {
        throw std::runtime_error("Invalid SDK event status");
      }

      if (callback) {
        callback(id, message->value);
      }
    }
  } catch (...) {
    state->Fail(std::current_exception());
    throw;
  }
}

void SdkConnection::Cancel(InvocationId id) {
  const auto state = impl_->state;
  {
    const std::lock_guard lock(state->mutex);
    state->Check();
    const auto it = state->calls.find(id);
    if (state->side != Side::Client || it == state->calls.end()) {
      return;
    }

    it->second->cancelled = true;
    state->changed.notify_all();
  }

  state->Queue(MakeFragment(id, MessageKind::Cancel, Operation{}, 0, 0));
}

void SdkConnection::PushInput(InvocationId id, const Json& value) {
  const auto state = impl_->state;
  const auto call = state->InputInvocation(id);
  const std::lock_guard lock(call->input_mutex);
  if (call->input_finished) {
    throw std::runtime_error("SDK input already finished");
  }

  state->Send(call, MessageKind::Input, Message{value, 0, {}});
}

void SdkConnection::FinishInput(InvocationId id) {
  const auto state = impl_->state;
  const auto call = state->InputInvocation(id);
  const std::lock_guard lock(call->input_mutex);
  if (!call->input_finished) {
    state->Send(call, MessageKind::Finish, Message{nullptr, 0, {}});
    call->input_finished = true;
  }
}

std::shared_ptr<SdkConnection::IncomingCall> SdkConnection::Accept() {
  const auto state = impl_->state;
  std::unique_lock lock(state->mutex);
  if (state->side != Side::Server) {
    throw std::runtime_error("SDK Accept requires server side");
  }

  state->changed.wait(lock, [&] { return state->failure || !state->incoming.empty(); });
  if (state->failure) {
    return {};
  }

  auto call = std::move(state->incoming.front());
  state->incoming.pop_front();
  return std::shared_ptr<IncomingCall>(new IncomingCall(state, std::move(call)));
}

void SdkConnection::SetDisconnectHandler(std::function<void()> handler) {
  const auto state = impl_->state;
  std::function<void()> previous;
  {
    const std::lock_guard lock(state->mutex);
    previous = std::exchange(state->disconnect_handler, std::move(handler));
  }

  state->changed.notify_all();
}

void SdkConnection::Close() noexcept {
  impl_->state->Fail(std::make_exception_ptr(std::runtime_error("SDK connection closed")));
}

SdkConnection::IncomingCall::IncomingCall(std::shared_ptr<State> state, std::shared_ptr<Invocation> invocation)
    : state_(std::move(state)), invocation_(std::move(invocation)) {}

SdkConnection::IncomingCall::~IncomingCall() {
  bool abandoned{};
  {
    const std::lock_guard lock(state_->mutex);
    abandoned = !invocation_->complete && !state_->failure;
  }

  if (abandoned) {
    state_->Fail(std::make_exception_ptr(std::runtime_error("SDK invocation abandoned without terminal result")));
  }

  state_->Retire(invocation_);
}

SdkConnection::InvocationId SdkConnection::IncomingCall::Id() const noexcept { return invocation_->id; }
Operation SdkConnection::IncomingCall::Op() const noexcept { return invocation_->op; }
const Json& SdkConnection::IncomingCall::Payload() const noexcept { return invocation_->payload; }

bool SdkConnection::IncomingCall::IsCancelled() const {
  const std::lock_guard lock(state_->mutex);
  return invocation_->cancelled;
}

void SdkConnection::IncomingCall::SetCancelHandler(std::function<void()> handler) {
  std::function<void()> previous;
  {
    const std::lock_guard lock(state_->mutex);
    previous = std::exchange(invocation_->cancel_handler, std::move(handler));
    state_->QueueCancelHook(*invocation_);
  }

  state_->changed.notify_all();
}

void SdkConnection::IncomingCall::SendEvent(const Json& value) {
  const std::lock_guard lock(invocation_->send_mutex);
  if (invocation_->complete) {
    throw std::runtime_error("SDK invocation already completed");
  }

  state_->Send(invocation_, MessageKind::Event, Message{value, 0, {}});
}

void SdkConnection::IncomingCall::Complete(SdkResult result) {
  const std::lock_guard lock(invocation_->send_mutex);
  if (invocation_->complete) {
    throw std::runtime_error("SDK invocation already completed");
  }

  state_->Queue(MakeFragment(invocation_->id, MessageKind::InputClosed, Operation{}, 0, 0));
  state_->Send(invocation_, MessageKind::Result,
               Message{std::move(result.value), result.status, std::move(result.error)});
  {
    const std::lock_guard state_lock(state_->mutex);
    invocation_->complete = true;
  }

  state_->Retire(invocation_);
}

std::optional<Json> SdkConnection::IncomingCall::ReadInput() {
  const std::lock_guard lock(invocation_->input_mutex);
  if (invocation_->input_finished) {
    return std::nullopt;
  }

  Assembly assembly;
  try {
    while (true) {
      auto frame = state_->Receive(invocation_);
      auto message = assembly.Add(frame);
      if (!message) {
        continue;
      }

      if (message->status != 0 || !message->error.empty()) {
        throw std::runtime_error("Invalid SDK input status");
      }

      if (Parse(frame).kind == MessageKind::Finish) {
        invocation_->input_finished = true;
        return std::nullopt;
      }

      return std::move(message->value);
    }
  } catch (const InvocationCancelled&) {
    throw;
  } catch (...) {
    state_->Fail(std::current_exception());
    throw;
  }
}

}  // namespace fl::hosting
