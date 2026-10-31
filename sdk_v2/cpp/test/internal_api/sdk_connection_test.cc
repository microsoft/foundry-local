// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/sdk_connection.h"
#include "utils/temp_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fl::hosting {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

template <typename T>
void Await(std::future<T>& future) {
  if (future.wait_for(10s) != std::future_status::ready) {
    throw std::runtime_error("SDK connection test deadline exceeded");
  }
}

void Await(const std::shared_future<void>& future) {
  if (future.wait_for(10s) != std::future_status::ready) {
    throw std::runtime_error("SDK connection test barrier deadline exceeded");
  }

  future.get();
}

Frame WireFrame(std::uint64_t id, std::uint8_t kind, std::uint16_t op = 0,
                const std::vector<std::uint8_t>& payload = {}, std::optional<std::uint64_t> declared_total = {}) {
  Frame frame{.invocation_id = id,
              .kind = kind >= 6 ? FrameKind::Control : FrameKind::Data,
              .payload = std::vector<std::byte>(20 + payload.size())};
  frame.payload[0] = std::byte{1};
  frame.payload[1] = static_cast<std::byte>(kind);
  frame.payload[2] = static_cast<std::byte>(op & 0xff);
  frame.payload[3] = static_cast<std::byte>(op >> 8);
  auto length = declared_total.value_or(payload.size());
  for (std::size_t i = 4; i < 12; ++i) {
    frame.payload[i] = static_cast<std::byte>(length & 0xff);
    length >>= 8;
  }

  std::transform(payload.begin(), payload.end(), frame.payload.begin() + 20,
                 [](auto byte) { return static_cast<std::byte>(byte); });
  return frame;
}

class SdkConnectionTest : public testing::Test {
 protected:
  void SetUp() override {
    static std::atomic<unsigned> sequence{};
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = getpid();
#endif
    const auto suffix = std::to_string(pid) + "-" + std::to_string(sequence.fetch_add(1));
#ifdef _WIN32
    endpoint_ = LR"(\\.\pipe\foundry-sdk-test-)" + std::filesystem::path(suffix).native();
#else
    directory_ = test::MakePrivateIpcDirectory();
    endpoint_ = directory_ / "s";
#endif
  }

  void TearDown() override {
    client_.reset();
    server_.reset();
    if (!directory_.empty()) {
      std::error_code error;
      std::filesystem::remove_all(directory_, error);
    }
  }

  std::pair<Channel, Channel> Channels() {
    Listener listener(endpoint_);
    auto client = Connect(endpoint_);
    auto [server, peer] = listener.Accept();
    static_cast<void>(peer);
    return {std::move(client), std::move(server)};
  }

  void Start() {
    auto [client, server] = Channels();
    client_ = std::make_unique<SdkConnection>(std::move(client), SdkConnection::Side::Client);
    server_ = std::make_unique<SdkConnection>(std::move(server), SdkConnection::Side::Server);
  }

  template <typename T>
  void Ready(std::future<T>& future) {
    if (future.wait_for(5s) != std::future_status::ready) {
      if (client_) {
        client_->Close();
      }

      if (server_) {
        server_->Close();
      }

      ADD_FAILURE() << "SDK connection operation exceeded its outer deadline";
    }

    Await(future);
  }

  void Ready(const std::shared_future<void>& future) {
    if (future.wait_for(5s) != std::future_status::ready) {
      if (client_) {
        client_->Close();
      }

      if (server_) {
        server_->Close();
      }

      ADD_FAILURE() << "SDK connection barrier exceeded its outer deadline";
    }

    Await(future);
  }

  std::filesystem::path endpoint_;
  std::filesystem::path directory_;
  std::unique_ptr<SdkConnection> client_;
  std::unique_ptr<SdkConnection> server_;
};

TEST_F(SdkConnectionTest, SimultaneousCallsReceiveTheirOwnOutOfOrderResults) {
  Start();
  auto dispatch = std::async(std::launch::async, [&] {
    std::vector<std::shared_ptr<SdkConnection::IncomingCall>> calls;
    for (int i = 0; i < 8; ++i) {
      auto call = server_->Accept();
      if (!call) {
        throw std::runtime_error("Missing admitted call");
      }

      calls.push_back(std::move(call));
    }

    for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
      (*it)->Complete({100 + (*it)->Payload().at("n").get<int>(), (*it)->Payload(), "result"});
    }
  });
  std::vector<std::future<SdkResult>> results;
  for (int i = 0; i < 8; ++i) {
    results.push_back(
        std::async(std::launch::async, [&, i] { return client_->Call(Operation::Observe, Json{{"n", 11 + i}}); }));
  }

  for (int i = 0; i < 8; ++i) {
    Ready(results[i]);
    const auto result = results[i].get();
    EXPECT_EQ(result.value, (Json{{"n", 11 + i}}));
    EXPECT_EQ(result.status, 111 + i);
    EXPECT_EQ(result.error, "result");
  }

  Ready(dispatch);
  dispatch.get();
}

TEST_F(SdkConnectionTest, CancellationBeforeCallSubmissionReachesAdmission) {
  Start();
  const auto id = client_->AllocateInvocation();
  client_->Cancel(id);
  auto dispatch = std::async(std::launch::async, [&] {
    auto call = server_->Accept();
    if (!call) {
      throw std::runtime_error("Missing cancelled call");
    }

    call->Complete({0, call->IsCancelled(), {}});
  });
  auto response = std::async(std::launch::async, [&] { return client_->Call(id, Operation::Observe, Json::object()); });
  Ready(response);
  EXPECT_EQ(response.get().value, true);
  Ready(dispatch);
  dispatch.get();
}

TEST_F(SdkConnectionTest, TerminalResultUnblocksLateInputBeforeTheCallbackReturnsWithoutClosingOtherCalls) {
  Start();
  auto dispatch = std::async(std::launch::async, [&] {
    auto call = server_->Accept();
    if (!call) {
      throw std::runtime_error("Missing streaming call");
    }

    call->SendEvent("delivered");
    call->Complete({0, "finished", {}});
    auto next = server_->Accept();
    if (!next) {
      throw std::runtime_error("Missing subsequent call");
    }

    next->Complete({0, "still connected", {}});
  });
  auto response = std::async(std::launch::async, [&] {
    return client_->Call(Operation::ProcessRequest, Json::object(), [&](auto id, const Json&) {
      EXPECT_THROW(client_->PushInput(id, "late acknowledgment"), SdkInputClosed);
    });
  });
  Ready(response);
  EXPECT_EQ(response.get().value, "finished");
  auto next = std::async(std::launch::async, [&] { return client_->Call(Operation::Observe, Json::object()); });
  Ready(next);
  EXPECT_EQ(next.get().value, "still connected");
  Ready(dispatch);
  dispatch.get();
}

TEST_F(SdkConnectionTest, OrderedCallbackCanMakeNestedSynchronousCallAndCancelItsOwnInvocation) {
  Start();
  std::promise<void> cancelled;
  auto cancellation = cancelled.get_future().share();
  auto dispatch = std::async(std::launch::async, [&] {
    auto stream = server_->Accept();
    if (!stream) {
      throw std::runtime_error("Missing stream");
    }

    stream->SetCancelHandler([&] { cancelled.set_value(); });
    auto execution = std::async(std::launch::async, [stream] {
      stream->SendEvent(41);
      stream->SendEvent(42);
      stream->Complete({-7, "finished", "cancelled"});
    });
    auto nested = server_->Accept();
    if (!nested) {
      throw std::runtime_error("Missing nested call");
    }

    EXPECT_EQ(nested->Op(), Operation::Observe);
    nested->Complete({0, 99, {}});
    Await(execution);
    execution.get();
  });
  std::vector<int> events;
  const auto calling_thread = std::this_thread::get_id();
  auto result = std::async(std::launch::async, [&] {
    const auto callback_thread = std::this_thread::get_id();
    return client_->Call(Operation::ProcessRequest, nullptr, [&](auto id, const Json& value) {
      EXPECT_EQ(std::this_thread::get_id(), callback_thread);
      EXPECT_NE(std::this_thread::get_id(), calling_thread);
      events.push_back(value.get<int>());
      if (value == 41) {
        EXPECT_EQ(client_->Call(Operation::Observe, nullptr).value, 99);
      } else {
        client_->Cancel(id);
        Await(cancellation);
      }
    });
  });
  Ready(result);
  Ready(dispatch);
  const auto terminal = result.get();
  dispatch.get();
  EXPECT_EQ(events, (std::vector<int>{41, 42}));
  EXPECT_EQ(terminal.status, -7);
  EXPECT_EQ(terminal.error, "cancelled");
  EXPECT_EQ(terminal.value, "finished");
}

TEST_F(SdkConnectionTest, LargeOwnedBinaryRequestEventAndResultCrossFrameBoundaries) {
  Start();
  std::vector<std::uint8_t> bytes(3 * kMaxFramePayload + 123);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(i % 251);
  }

  const auto payload = Json::binary(bytes);
  auto dispatch = std::async(std::launch::async, [&] {
    auto call = server_->Accept();
    if (!call) {
      throw std::runtime_error("Missing large request");
    }

    EXPECT_EQ(call->Payload(), payload);
    call->SendEvent(call->Payload());
    call->Complete({0, call->Payload(), {}});
  });
  auto result = std::async(std::launch::async, [&] {
    return client_->Call(Operation::ProcessRequest, payload, [&](auto, const Json& event) {
      EXPECT_TRUE(event.is_binary());
      EXPECT_EQ(event.get_binary(), payload.get_binary());
    });
  });
  Ready(result);
  Ready(dispatch);
  EXPECT_EQ(result.get().value, payload);
  dispatch.get();
}

TEST_F(SdkConnectionTest, OversizedDeclaredMessageClosesOnlyItsConnectionWithoutAllocatingThePayload) {
  auto [peer, server] = Channels();
  server_ = std::make_unique<SdkConnection>(std::move(server), SdkConnection::Side::Server);
  auto accept = std::async(std::launch::async, [&] { return server_->Accept(); });
  peer.WriteFrame(WireFrame(1, 1, static_cast<std::uint16_t>(Operation::Observe), {0xa3},
                            SdkConnection::kMaxMessageBytes + 1));
  Ready(accept);
  EXPECT_EQ(accept.get(), nullptr);
}

TEST_F(SdkConnectionTest, PendingRequestBudgetIncludesIncompleteDeclarations) {
  auto [peer, server] = Channels();
  server_ = std::make_unique<SdkConnection>(std::move(server), SdkConnection::Side::Server);
  auto accept = std::async(std::launch::async, [&] { return server_->Accept(); });
  const auto total = SdkConnection::kMaxMessageBytes / 2 + 1;
  for (std::uint64_t id = 1; id <= 2; ++id) {
    peer.WriteFrame(WireFrame(id, 1, static_cast<std::uint16_t>(Operation::Observe), {0xa3}, total));
  }

  Ready(accept);
  EXPECT_EQ(accept.get(), nullptr);
}

TEST_F(SdkConnectionTest, TooManyIncompleteRequestsCloseTheConnection) {
  auto [peer, server] = Channels();
  server_ = std::make_unique<SdkConnection>(std::move(server), SdkConnection::Side::Server);
  auto accept = std::async(std::launch::async, [&] { return server_->Accept(); });
  for (std::uint64_t id = 1; id <= SdkConnection::kMaxPendingCalls + 1; ++id) {
    peer.WriteFrame(WireFrame(id, 1, static_cast<std::uint16_t>(Operation::Observe), {0xa3}, 2));
  }

  Ready(accept);
  EXPECT_EQ(accept.get(), nullptr);
}

TEST_F(SdkConnectionTest, LocalCallLimitDoesNotCloseTheConnectionAndCompletionReleasesCapacity) {
  Start();
  std::vector<std::uint64_t> ids;
  for (std::size_t i = 0; i < SdkConnection::kMaxPendingCalls; ++i) {
    ids.push_back(client_->AllocateInvocation());
  }

  EXPECT_THROW(static_cast<void>(client_->AllocateInvocation()), std::runtime_error);
  auto dispatch = std::async(std::launch::async, [&] {
    for (int i = 0; i < 2; ++i) {
      auto call = server_->Accept();
      if (!call) {
        throw std::runtime_error("Call limit incorrectly closed the connection");
      }

      call->Complete({0, i, {}});
    }
  });
  auto first = std::async(std::launch::async, [&] { return client_->Call(ids.front(), Operation::Observe, nullptr); });
  Ready(first);
  EXPECT_EQ(first.get().value, 0);
  const auto replacement = client_->AllocateInvocation();
  auto second = std::async(std::launch::async, [&] { return client_->Call(replacement, Operation::Observe, nullptr); });
  Ready(second);
  Ready(dispatch);
  EXPECT_EQ(second.get().value, 1);
  dispatch.get();
}

TEST_F(SdkConnectionTest, ResourceReleaseHasCapacityWhenAllOrdinaryClientCallSlotsAreReserved) {
  Start();
  for (std::size_t i = 0; i < SdkConnection::kMaxPendingCalls; ++i) {
    static_cast<void>(client_->AllocateInvocation());
  }

  auto dispatch = std::async(std::launch::async, [&] {
    for (const auto operation : {Operation::ReleaseSession, Operation::ReleasePreflight}) {
      auto call = server_->Accept();
      if (!call) {
        throw std::runtime_error("Missing resource retirement");
      }

      EXPECT_EQ(call->Op(), operation);
      call->Complete({0, nullptr, {}});
    }
  });
  for (const auto operation : {Operation::ReleaseSession, Operation::ReleasePreflight}) {
    auto release = std::async(std::launch::async, [&] { return client_->Call(operation, nullptr); });
    Ready(release);
    EXPECT_NO_THROW({ EXPECT_EQ(release.get().status, 0); });
    EXPECT_THROW(static_cast<void>(client_->AllocateInvocation()), std::runtime_error);
  }

  Ready(dispatch);
  dispatch.get();
}

TEST_F(SdkConnectionTest, CompletionRestoresRequestBudgetAndRetirementBypassesSaturatedInboundBudgets) {
  auto [peer, server] = Channels();
  server_ = std::make_unique<SdkConnection>(std::move(server), SdkConnection::Side::Server);
  std::promise<void> completed;
  auto completion = completed.get_future().share();
  auto dispatch = std::async(std::launch::async, [&] {
    auto first = server_->Accept();
    if (!first) {
      throw std::runtime_error("Missing initial request");
    }

    first->Complete({0, nullptr, {}});
    completed.set_value();
    for (const auto operation : {Operation::ReleaseSession, Operation::ReleasePreflight}) {
      auto call = server_->Accept();
      if (!call) {
        throw std::runtime_error("Saturated inbound budgets rejected resource retirement");
      }

      EXPECT_EQ(call->Op(), operation);
      call->Complete({0, nullptr, {}});
    }
  });
  auto sender = std::async(std::launch::async, [&] {
    const auto request = Json::to_cbor(Json{{"value", nullptr}, {"status", 0}, {"error", ""}});
    auto receive_result = [&](std::uint64_t id) {
      EXPECT_EQ(peer.ReadFrame().payload[1], std::byte{9});
      EXPECT_EQ(peer.ReadFrame().payload[1], std::byte{8});
      peer.WriteFrame(WireFrame(id, 7));
      EXPECT_EQ(peer.ReadFrame().payload[1], std::byte{3});
    };
    peer.WriteFrame(WireFrame(1, 1, static_cast<std::uint16_t>(Operation::Observe), request));
    receive_result(1);
    Await(completion);
    std::uint64_t id = 2;
    for (std::size_t i = 0; i < SdkConnection::kMaxPendingCalls; ++i) {
      peer.WriteFrame(WireFrame(id++, 1, static_cast<std::uint16_t>(Operation::Observe), {0xa3},
                                SdkConnection::kMaxMessageBytes / SdkConnection::kMaxPendingCalls));
    }

    for (const auto operation : {Operation::ReleaseSession, Operation::ReleasePreflight}) {
      peer.WriteFrame(WireFrame(id, 1, static_cast<std::uint16_t>(operation), request));
      receive_result(id++);
    }
  });
  Ready(sender);
  Ready(dispatch);
  sender.get();
  dispatch.get();
}

TEST_F(SdkConnectionTest, OversizedRetirementCannotBypassThePendingRequestBudget) {
  auto [peer, server] = Channels();
  server_ = std::make_unique<SdkConnection>(std::move(server), SdkConnection::Side::Server);
  auto accept = std::async(std::launch::async, [&] { return server_->Accept(); });
  peer.WriteFrame(WireFrame(1, 1, static_cast<std::uint16_t>(Operation::ReleaseSession), {0xa3},
                            SdkConnection::kMaxReleaseRequestBytes + 1));
  Ready(accept);
  EXPECT_EQ(accept.get(), nullptr);
}

TEST_F(SdkConnectionTest, InputPushAndFinishProceedWhileOutputCallbackIsBackpressured) {
  Start();
  std::promise<void> callback_entered;
  auto entered = callback_entered.get_future().share();
  std::promise<void> release_callback;
  auto release = release_callback.get_future().share();
  std::promise<std::shared_ptr<SdkConnection::IncomingCall>> admitted;
  auto admission = admitted.get_future();
  const auto id = client_->AllocateInvocation();
  auto dispatch = std::async(std::launch::async, [&] {
    auto call = server_->Accept();
    if (!call) {
      throw std::runtime_error("Missing input call");
    }

    admitted.set_value(call);
    call->SendEvent("blocked callback");
    call->SendEvent("after input");
    call->Complete({0, true, {}});
  });
  std::vector<std::string> events;
  auto result = std::async(std::launch::async, [&] {
    return client_->Call(id, Operation::ProcessRequest, nullptr, [&](auto, const Json& event) {
      events.push_back(event.get<std::string>());
      if (events.size() == 1) {
        callback_entered.set_value();
        Await(release);
      }
    });
  });
  Ready(admission);
  auto call = admission.get();
  Ready(entered);
  const auto input = Json::binary(std::vector<std::uint8_t>(2 * kMaxFramePayload + 17, 0xa5));
  auto producer = std::async(std::launch::async, [&] {
    client_->PushInput(id, input);
    client_->FinishInput(id);
  });
  auto consumer = std::async(std::launch::async, [&] {
    EXPECT_EQ(call->ReadInput(), std::optional<Json>(input));
    EXPECT_FALSE(call->ReadInput().has_value());
    EXPECT_FALSE(call->ReadInput().has_value());
  });
  Ready(producer);
  Ready(consumer);
  release_callback.set_value();
  Ready(result);
  Ready(dispatch);
  producer.get();
  consumer.get();
  dispatch.get();
  EXPECT_EQ(result.get().value, true);
  EXPECT_EQ(events, (std::vector<std::string>{"blocked callback", "after input"}));
}

TEST_F(SdkConnectionTest, EarlyInputProducerWaitsForLargeInitialRequestBeforeSendingControl) {
  Start();
  const auto id = client_->AllocateInvocation();
  std::promise<void> producer_started;
  auto started = producer_started.get_future().share();
  const auto payload = Json::binary(std::vector<std::uint8_t>(4 * 1024 * 1024, 0x37));
  auto producer = std::async(std::launch::async, [&] {
    producer_started.set_value();
    client_->PushInput(id, "first input");
    client_->FinishInput(id);
  });
  Ready(started);
  auto dispatch = std::async(std::launch::async, [&] {
    auto call = server_->Accept();
    if (!call) {
      throw std::runtime_error("Early input overtook initial request");
    }

    EXPECT_EQ(call->Payload(), payload);
    EXPECT_EQ(call->ReadInput(), std::optional<Json>("first input"));
    EXPECT_FALSE(call->ReadInput().has_value());
    call->Complete({0, "input consumed", {}});
  });
  auto result = std::async(std::launch::async, [&] { return client_->Call(id, Operation::ProcessRequest, payload); });
  Ready(producer);
  Ready(result);
  Ready(dispatch);
  producer.get();
  dispatch.get();
  EXPECT_EQ(result.get().value, "input consumed");
}

TEST_F(SdkConnectionTest, DisconnectUnblocksBackpressuredOutputAndCallerAndRunsCancellationHook) {
  Start();
  std::promise<void> entered_callback;
  auto entered = entered_callback.get_future().share();
  std::promise<void> release_callback;
  auto release = release_callback.get_future().share();
  std::promise<void> cancelled;
  auto cancellation = cancelled.get_future().share();
  std::promise<void> disconnected;
  auto disconnection = disconnected.get_future().share();
  server_->SetDisconnectHandler([&] { disconnected.set_value(); });
  auto dispatch = std::async(std::launch::async, [&] {
    auto call = server_->Accept();
    if (!call) {
      throw std::runtime_error("Missing backpressured call");
    }

    call->SetCancelHandler([&] { cancelled.set_value(); });
    call->SendEvent(1);
    try {
      call->SendEvent(2);
      return false;
    } catch (const std::exception&) {
      return true;
    }
  });
  auto result = std::async(std::launch::async, [&] {
    try {
      static_cast<void>(client_->Call(Operation::ProcessRequest, nullptr, [&](auto, const Json&) {
        entered_callback.set_value();
        Await(release);
      }));
      return false;
    } catch (const std::exception&) {
      return true;
    }
  });
  Ready(entered);
  client_->Close();
  Ready(cancellation);
  Ready(disconnection);
  Ready(dispatch);
  EXPECT_TRUE(dispatch.get());
  release_callback.set_value();
  Ready(result);
  EXPECT_TRUE(result.get());
}

TEST_F(SdkConnectionTest, CancelOldInvocationCannotCancelNewUseOfSameSdkRequestIdentity) {
  Start();
  std::atomic<unsigned> cancellations{};
  std::promise<void> first_cancelled;
  auto cancellation = first_cancelled.get_future().share();
  const auto old_id = client_->AllocateInvocation();
  auto dispatch = std::async(std::launch::async, [&] {
    for (int i = 0; i < 2; ++i) {
      auto call = server_->Accept();
      if (!call) {
        throw std::runtime_error("Missing reused SDK request");
      }

      EXPECT_EQ(call->Payload(), (Json{{"request", 7}}));
      call->SetCancelHandler([&, i] {
        ++cancellations;
        if (i == 0) {
          first_cancelled.set_value();
        }
      });
      call->SendEvent(i);
      if (i == 0) {
        Await(cancellation);
      }

      call->Complete({0, call->IsCancelled(), {}});
    }
  });
  auto results = std::async(std::launch::async, [&] {
    const auto first = client_->Call(old_id, Operation::ProcessRequest, Json{{"request", 7}},
                                     [&](auto id, const Json&) { client_->Cancel(id); });
    const auto second = client_->Call(Operation::ProcessRequest, Json{{"request", 7}},
                                      [&](auto, const Json&) { client_->Cancel(old_id); });
    return std::pair{first, second};
  });
  Ready(results);
  Ready(dispatch);
  const auto [first, second] = results.get();
  dispatch.get();
  EXPECT_EQ(first.value, true);
  EXPECT_EQ(second.value, false);
  EXPECT_EQ(cancellations.load(), 1u);
  EXPECT_THROW(static_cast<void>(client_->Call(old_id, Operation::Observe, nullptr)), std::runtime_error);
}

TEST_F(SdkConnectionTest, CancelUnblocksPendingInputWithoutFailingOtherInvocations) {
  Start();
  std::promise<void> reading;
  auto started = reading.get_future().share();
  const auto id = client_->AllocateInvocation();
  auto dispatch = std::async(std::launch::async, [&] {
    auto call = server_->Accept();
    if (!call) {
      throw std::runtime_error("Missing input waiter");
    }

    reading.set_value();
    EXPECT_THROW(static_cast<void>(call->ReadInput()), std::runtime_error);
    call->Complete({-1, nullptr, "cancelled"});
    auto observation = server_->Accept();
    if (!observation) {
      throw std::runtime_error("Missing subsequent observation");
    }

    observation->Complete({0, 123, {}});
  });
  auto result = std::async(std::launch::async, [&] { return client_->Call(id, Operation::ProcessRequest, nullptr); });
  Ready(started);
  client_->Cancel(id);
  Ready(result);
  EXPECT_EQ(result.get().status, -1);
  auto observation = std::async(std::launch::async, [&] { return client_->Call(Operation::Observe, nullptr); });
  Ready(observation);
  Ready(dispatch);
  EXPECT_EQ(observation.get().value, 123);
  dispatch.get();
}

TEST_F(SdkConnectionTest, MalformedResponseTerminallyFailsAllWaitersWithoutDeclaredLengthAllocation) {
  auto [client, peer] = Channels();
  client_ = std::make_unique<SdkConnection>(std::move(client), SdkConnection::Side::Client);
  const auto id = client_->AllocateInvocation();
  auto result = std::async(std::launch::async, [&] { return client_->Call(id, Operation::Observe, nullptr); });
  auto other = std::async(std::launch::async, [&] { return client_->Call(Operation::LoadedModels, nullptr); });
  auto corrupt = std::async(std::launch::async, [&] {
    static_cast<void>(peer.ReadFrame());
    Frame malformed{.invocation_id = id, .kind = FrameKind::Data, .payload = std::vector<std::byte>(20)};
    malformed.payload[0] = std::byte{1};
    malformed.payload[1] = std::byte{3};
    std::fill(malformed.payload.begin() + 4, malformed.payload.begin() + 12, std::byte{0xff});
    // An empty fragment with a nonzero length is invalid before any application allocation.
    peer.WriteFrame(malformed);
  });
  Ready(corrupt);
  Ready(result);
  Ready(other);
  corrupt.get();
  EXPECT_THROW(static_cast<void>(result.get()), std::runtime_error);
  EXPECT_THROW(static_cast<void>(other.get()), std::runtime_error);
  EXPECT_THROW(static_cast<void>(client_->Call(Operation::Observe, nullptr)), std::runtime_error);
}

TEST_F(SdkConnectionTest, CancelBeforeRequestAndAdmissionIsRetainedAndHookRunsOutsideReader) {
  auto [peer, server] = Channels();
  server_ = std::make_unique<SdkConnection>(std::move(server), SdkConnection::Side::Server);
  std::promise<void> cancelled;
  auto cancellation = cancelled.get_future().share();
  auto dispatch = std::async(std::launch::async, [&] {
    auto call = server_->Accept();
    if (!call) {
      throw std::runtime_error("Missing pre-cancelled call");
    }

    EXPECT_EQ(call->Id(), 31u);
    EXPECT_TRUE(call->IsCancelled());
    call->SetCancelHandler([&] { cancelled.set_value(); });
    Await(cancellation);
    call->Complete({-1, "not executed", "cancelled"});
  });
  peer.WriteFrame(WireFrame(31, 6));
  const auto request = Json::to_cbor(Json{{"value", nullptr}, {"status", 0}, {"error", ""}});
  peer.WriteFrame(WireFrame(31, 1, static_cast<std::uint16_t>(Operation::ProcessRequest), request));
  auto terminal = std::async(std::launch::async, [&] {
    EXPECT_EQ(peer.ReadFrame().payload[1], std::byte{9});
    EXPECT_EQ(peer.ReadFrame().payload[1], std::byte{8});
    peer.WriteFrame(WireFrame(31, 7));
    auto frame = peer.ReadFrame();
    std::vector<std::uint8_t> bytes;
    for (auto it = frame.payload.begin() + 20; it != frame.payload.end(); ++it) {
      bytes.push_back(static_cast<std::uint8_t>(*it));
    }

    return Json::from_cbor(bytes);
  });
  Ready(terminal);
  Ready(dispatch);
  EXPECT_EQ(terminal.get(), (Json{{"value", "not executed"}, {"status", -1}, {"error", "cancelled"}}));
  dispatch.get();
}

TEST_F(SdkConnectionTest, MissingRequiredCborFieldsClosesServerAndWakesAccept) {
  auto [peer, server] = Channels();
  server_ = std::make_unique<SdkConnection>(std::move(server), SdkConnection::Side::Server);
  auto accept = std::async(std::launch::async, [&] { return server_->Accept(); });
  peer.WriteFrame(
      WireFrame(1, 1, static_cast<std::uint16_t>(Operation::Handshake), Json::to_cbor(Json{{"value", nullptr}})));
  if (accept.wait_for(5s) != std::future_status::ready) {
    server_->Close();
    peer.Close();
    ADD_FAILURE() << "Malformed CBOR did not wake Accept";
  }

  Await(accept);
  EXPECT_EQ(accept.get(), nullptr);
}

TEST_F(SdkConnectionTest, CancelledPartialEventCanBeReplacedByTerminalWithoutDeliveringPartialValue) {
  auto [client, peer] = Channels();
  client_ = std::make_unique<SdkConnection>(std::move(client), SdkConnection::Side::Client);
  const auto id = client_->AllocateInvocation();
  std::promise<void> partial_consumed;
  auto partial = partial_consumed.get_future().share();
  std::atomic<unsigned> events{};
  auto result = std::async(std::launch::async, [&] {
    return client_->Call(id, Operation::ProcessRequest, nullptr, [&](auto, const Json&) { ++events; });
  });
  auto sender = std::async(std::launch::async, [&] {
    EXPECT_EQ(peer.ReadFrame().payload[1], std::byte{1});
    peer.WriteFrame(WireFrame(id, 8));
    EXPECT_EQ(peer.ReadFrame().payload[1], std::byte{7});
    auto fragment = WireFrame(id, 2, 0, {0xa3});
    fragment.payload[4] = std::byte{100};
    peer.WriteFrame(fragment);
    peer.WriteFrame(WireFrame(id, 8));
    EXPECT_EQ(peer.ReadFrame().payload[1], std::byte{7});
    partial_consumed.set_value();
    EXPECT_EQ(peer.ReadFrame().payload[1], std::byte{6});
    const auto terminal = Json::to_cbor(Json{{"value", nullptr}, {"status", -1}, {"error", "cancelled"}});
    peer.WriteFrame(WireFrame(id, 3, 0, terminal));
  });
  Ready(partial);
  client_->Cancel(id);
  Ready(result);
  Ready(sender);
  const auto terminal = result.get();
  sender.get();
  EXPECT_EQ(terminal.status, -1);
  EXPECT_EQ(terminal.error, "cancelled");
  EXPECT_EQ(events.load(), 0u);
}

TEST_F(SdkConnectionTest, DisconnectReleasesWriterBlockedByPeerThatDoesNotReadRequests) {
  auto [client, peer] = Channels();
  client_ = std::make_unique<SdkConnection>(std::move(client), SdkConnection::Side::Client);
  std::promise<void> first_fragment;
  auto observed = first_fragment.get_future().share();
  auto result = std::async(std::launch::async, [&] {
    try {
      const auto bytes = Json::binary(std::vector<std::uint8_t>(4 * 1024 * 1024, 17));
      static_cast<void>(client_->Call(Operation::RegisterModel, bytes));
      return false;
    } catch (const std::exception&) {
      return true;
    }
  });
  auto read_one = std::async(std::launch::async, [&] {
    const auto frame = peer.ReadFrame();
    EXPECT_EQ(frame.payload.size(), kMaxFramePayload);
    first_fragment.set_value();
  });
  Ready(observed);
  peer.Close();
  Ready(read_one);
  Ready(result);
  read_one.get();
  EXPECT_TRUE(result.get());
}

}  // namespace
}  // namespace fl::hosting
