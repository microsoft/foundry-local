// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "hosting/ipc_channel.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace fl::hosting {

enum class Operation : std::uint16_t {
  Handshake = 1,
  Observe,
  WebServiceStart,
  WebServiceUrls,
  WebServiceStop,
  DiscoverableEps,
  DownloadAndRegisterEps,
  RegisterModel,
  UnregisterModel,
  LoadedModels,
  IsLoaded,
  Load,
  Unload,
  ModelInputOutputInfo,
  CreateSession,
  ReleaseSession,
  SessionOptions,
  AddTool,
  RemoveTool,
  UndoTurns,
  CapturePreflight,
  ExecutePreflight,
  ReleasePreflight,
  ProcessRequest,
  RemoveFromCache,
  RuntimeDevices
};

constexpr bool IsResourceRelease(Operation operation) noexcept {
  return operation == Operation::ReleaseSession || operation == Operation::ReleasePreflight;
}

struct SdkResult {
  std::int32_t status{};
  nlohmann::json value;
  std::string error;
};

class SdkInputClosed : public std::runtime_error {
 public:
  SdkInputClosed() : std::runtime_error("SDK invocation no longer accepts input") {}
};

// Protocol errors and transport loss throw; SDK status remains an uninterpreted signed 32-bit value.
class SdkConnection {
 private:
  struct State;
  struct Invocation;
  struct Impl;

 public:
  enum class Side { Client,
                    Server };
  static constexpr std::uint8_t kProtocolVersion = 1;
  using InvocationId = std::uint64_t;
  using EventCallback = std::function<void(InvocationId, const nlohmann::json&)>;

  class IncomingCall {
   public:
    ~IncomingCall();
    IncomingCall(const IncomingCall&) = delete;
    IncomingCall& operator=(const IncomingCall&) = delete;

    [[nodiscard]] InvocationId Id() const noexcept;
    [[nodiscard]] Operation Op() const noexcept;
    [[nodiscard]] const nlohmann::json& Payload() const noexcept;
    [[nodiscard]] bool IsCancelled() const;

    // Hooks run once, outside transport locks, on the connection's control worker. They must promptly cancel/unblock.
    // Register after acquiring the admitted resource graph; keep that graph captured by the worker and/or this handle.
    void SetCancelHandler(std::function<void()> handler);
    void SendEvent(const nlohmann::json& value);
    void Complete(SdkResult result);
    [[nodiscard]] std::optional<nlohmann::json> ReadInput();

   private:
    IncomingCall(std::shared_ptr<State> state, std::shared_ptr<Invocation> invocation);
    std::shared_ptr<State> state_;
    std::shared_ptr<Invocation> invocation_;
    friend class SdkConnection;
  };

  explicit SdkConnection(Channel channel, Side side);
  ~SdkConnection();
  SdkConnection(const SdkConnection&) = delete;
  SdkConnection& operator=(const SdkConnection&) = delete;

  // IDs are allocated locally and cannot be reused. Allocate explicitly when another thread needs the cancel identity.
  [[nodiscard]] InvocationId AllocateInvocation();
  [[nodiscard]] SdkResult Call(InvocationId id, Operation op, const nlohmann::json& payload,
                               EventCallback callback = {});
  [[nodiscard]] SdkResult Call(Operation op, const nlohmann::json& payload, EventCallback callback = {});
  void Cancel(InvocationId id);
  // Producers may start with an allocated ID; input waits until the initial request is written.
  void PushInput(InvocationId id, const nlohmann::json& value);
  void FinishInput(InvocationId id);

  // Parent workers perform admission/dispatch outside the reader and check IsCancelled() before executing.
  // Null means terminal connection loss. Dropping an uncompleted call terminally closes this connection.
  [[nodiscard]] std::shared_ptr<IncomingCall> Accept();
  void SetDisconnectHandler(std::function<void()> handler);
  void Close() noexcept;

  // Limit outstanding calls and wire values as well as frame queues. Exceeding an inbound limit closes only this
  // connection. Initial requests share one message-sized budget until completion; live input uses separate messages.
  static constexpr std::size_t kMaxPendingCalls = 128;
  static constexpr std::size_t kMaxMessageBytes = 256 * 1024 * 1024;
  // Serialized resource retirement has a reserved call slot and a small independent request budget.
  static constexpr std::size_t kMaxReleaseRequestBytes = 4 * 1024;
  // Data FIFO and delivery budgets; the control FIFO has a separate 64 KiB budget.
  static constexpr std::size_t kTransportQueueBytes = 256 * 1024;
  static constexpr std::size_t kDeliveryQueueBytes = 256 * 1024;

 private:
  InvocationId AllocateInvocation(Operation operation);
  std::unique_ptr<Impl> impl_;
};

}  // namespace fl::hosting
