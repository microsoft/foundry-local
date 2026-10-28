// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "api/hosted_backend.h"

#include "api/request_admission.h"
#include "exception.h"
#include "hosting/sdk_values.h"
#include "inferencing/generative/chat/search_options.h"
#include "inferencing/session/callback_handler.h"
#include "util/scope_guard.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

namespace fl::api {
namespace {

using namespace hosting;
using Json = nlohmann::json;

Json Value(SdkResult result) {
  if (result.status != FOUNDRY_LOCAL_OK) {
    FL_THROW(static_cast<flErrorCode>(result.status), result.error);
  }

  return std::move(result.value);
}

struct CallbackScope {
  const void* session;
  uint64_t invocation;
  const CallbackScope* previous;
};

thread_local const CallbackScope* callback_scope = nullptr;

struct InputSource {
  ItemQueue* queue;
  uint64_t id;
  bool finished = false;
};

struct ClientRun {
  std::shared_ptr<HostedContext> context;
  std::atomic<uint64_t> invocation{0};
  std::atomic<bool> stopping{false};
  std::atomic<bool> wire_cancelled{false};
  std::vector<InputSource> inputs;
  std::filesystem::path working_directory;
  std::mutex mutex;
  std::condition_variable changed;
  std::exception_ptr input_error;

  void Stop() noexcept {
    {
      std::lock_guard lock(mutex);
      stopping.store(true, std::memory_order_release);
    }

    changed.notify_all();
  }

  void Cancel() noexcept {
    wire_cancelled.store(true, std::memory_order_release);
    Stop();
    try {
      if (const auto id = invocation.load(std::memory_order_acquire)) {
        context->Connection().Cancel(id);
      }
    } catch (...) {
      // The processing call reports transport failure; cancellation stays noexcept.
    }
  }

  void Pump() noexcept {
    {
      std::unique_lock lock(mutex);
      changed.wait(lock, [&] { return stopping.load() || invocation.load() != 0; });
    }

    try {
      while (!stopping.load(std::memory_order_acquire)) {
        bool pending = false;
        bool transferred = false;
        for (auto& source : inputs) {
          if (stopping.load(std::memory_order_acquire)) {
            return;
          }
          if (source.finished) {
            continue;
          }

          auto item = source.queue->TryPop();
          if (!item && source.queue->IsFinished()) {
            item = source.queue->TryPop();
            if (!item) {
              context->Connection().PushInput(invocation.load(), {{"kind", "finish_queue"}, {"queue", source.id}});
              source.finished = true;
              continue;
            }
          }

          pending = true;
          if (item) {
            auto encoded = EncodeItem(*item, {.caller_working_directory = working_directory});
            context->Connection().PushInput(invocation.load(),
                                            {{"kind", "item"}, {"queue", source.id}, {"item", std::move(encoded)}});
            transferred = true;
          }
        }

        if (!pending) {
          return;
        }
        if (!transferred) {
          std::unique_lock lock(mutex);
          changed.wait_for(lock, std::chrono::milliseconds(5), [&] { return stopping.load(); });
        }
      }
    } catch (const SdkInputClosed&) {
      Stop();
    } catch (...) {
      if (!stopping.load(std::memory_order_acquire)) {
        {
          std::lock_guard lock(mutex);
          input_error = std::current_exception();
        }

        Cancel();
      }
    }
  }
};

class HostedPreflight final : public PreflightBackend {
 public:
  HostedPreflight(std::shared_ptr<HostedContext> context, RemoteObjectIdentity identity, std::shared_ptr<void> owner)
      : PreflightBackend(BackendKind::Hosted),
        owner_(std::move(owner)),
        context_(std::move(context)),
        identity_(std::move(identity)) {
    context_->Validate(identity_);
  }

  ~HostedPreflight() override { context_->Release(Operation::ReleasePreflight, identity_); }

  RequestBudget Execute() override {
    auto context = context_;
    return DecodeBudget(context->Call(Operation::ExecutePreflight, {{"object", EncodeObjectIdentity(identity_)}}));
  }

 private:
  std::shared_ptr<void> owner_;
  std::shared_ptr<HostedContext> context_;
  const RemoteObjectIdentity identity_;
};

class HostedSession final : public SessionBackend {
 private:
  struct State {
    std::shared_ptr<void> owner;
    std::shared_ptr<HostedContext> context;
    RemoteObjectIdentity identity;
    std::mutex callback_mutex;
    StreamingCallback callback;
    std::atomic<size_t> turns{0};
  };

 public:
  HostedSession(std::shared_ptr<HostedContext> context, RemoteObjectIdentity identity, std::shared_ptr<void> owner)
      : SessionBackend(BackendKind::Hosted), state_(std::make_shared<State>()) {
    context->Validate(identity);
    state_->owner = std::move(owner);
    state_->context = std::move(context);
    state_->identity = std::move(identity);
  }

  ~HostedSession() override { state_->context->Release(Operation::ReleaseSession, state_->identity); }

  void SetStreamingCallback(flStreamingCallback callback, void* user_data) override {
    std::lock_guard lock(state_->callback_mutex);
    state_->callback = {callback, user_data};
  }

  void SetSessionOptions(const KeyValuePairs& options) override {
    Call(state_, Operation::SessionOptions, {{"options", EncodeOptions(options)}});
  }

  void AddToolDefinition(ToolDefinition definition) override {
    Call(state_, Operation::AddTool, {{"tool", EncodeToolDefinition(definition)}});
  }

  bool RemoveToolDefinition(const std::string& name) override {
    return Call(state_, Operation::RemoveTool, {{"name", name}}).get<bool>();
  }

  size_t TurnCount() const override { return state_->turns.load(std::memory_order_acquire); }

  void UndoTurns(size_t count) override {
    const auto state = state_;
    state->turns.store(Call(state, Operation::UndoTurns, {{"count", count}}).get<size_t>(), std::memory_order_release);
  }

  std::unique_ptr<PreflightBackend> CreateRequestPreflight(const Request& request) const override {
    const auto state = state_;
    const auto value =
        Call(state, Operation::CapturePreflight,
             {{"request", EncodeRequest(request, {.caller_working_directory = std::filesystem::current_path()})}});
    return std::make_unique<HostedPreflight>(state->context, DecodeObjectIdentity(value), state->owner);
  }

  void ProcessRequest(const Request& request, Response& response, std::optional<StreamingCallback> callback) override {
    const auto state = state_;
    for (auto* scope = callback_scope; scope; scope = scope->previous) {
      if (scope->session == state.get()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "session processing is unavailable from its streaming callback");
      }
    }

    auto run = std::make_shared<ClientRun>();
    run->context = state->context;
    RequestAdmission admission(request, [run](auto reason, const std::string&) {
      if (reason == Request::CancellationReason::StreamingCallback ||
          reason == Request::CancellationReason::StreamingCallbackException) {
        run->Stop();
      } else {
        run->Cancel();
      }
    });
    run->working_directory = std::filesystem::current_path();
    if (!callback) {
      std::lock_guard lock(state->callback_mutex);
      callback = state->callback;
    }

    auto payload = Payload(
        state, {{"request", EncodeRequest(request, {.caller_working_directory = run->working_directory,
                                                    .register_queue =
                                                        [&](const ItemQueue& queue) {
                                                          const auto id = static_cast<uint64_t>(run->inputs.size()) + 1;
                                                          run->inputs.push_back({const_cast<ItemQueue*>(&queue), id});
                                                          return id;
                                                        }})},
                {"streaming", static_cast<bool>(callback->function)}});
    ItemQueue output;
    uint64_t sequence = 0;
    SdkConnection::EventCallback deliver = [&](auto id, const Json& event) {
      const auto next = event.at("sequence").get<uint64_t>();
      if (!callback->function || event.at("kind") != "stream" || next != sequence + 1 ||
          !event.at("items").is_array()) {
        FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid SDK streaming event");
      }

      sequence = next;
      for (const auto& item : event.at("items")) {
        output.Push(DecodeItem(item, {}));
      }

      std::optional<std::string> exception;
      const CallbackScope scope{state.get(), id, callback_scope};
      callback_scope = &scope;
      ScopeGuard restore_scope([&]() noexcept { callback_scope = scope.previous; });
      while (output.Size() != 0) {
        if (request.IsCancellationRequested() && output.Size() > CallbackHandler::kMaxCancellationDrainItems) {
          while (output.TryPop()) {
          }

          break;
        }

        try {
          if (callback->function({FOUNDRY_LOCAL_DATA_VERSION, output.AsApiType()}, callback->user_data) != 0) {
            request.Cancel(Request::CancellationReason::StreamingCallback);
          }
        } catch (const std::exception& failure) {
          exception = failure.what();
        } catch (...) {
          exception = "non-standard exception";
        }

        if (exception) {
          request.CancelFromStreamingCallbackException(*exception);
          while (output.TryPop()) {
          }

          break;
        }
      }

      if (!run->wire_cancelled.load(std::memory_order_acquire)) {
        try {
          state->context->Connection().PushInput(
              id, {{"kind", "callback"},
                   {"sequence", sequence},
                   {"cancel", request.GetCancellationReason() == Request::CancellationReason::StreamingCallback},
                   {"exception", exception ? Json(*exception) : Json(nullptr)}});
        } catch (const SdkInputClosed&) {
          // The host may finish while user code is still handling an already-delivered callback.
        } catch (...) {
          if (!run->wire_cancelled.load(std::memory_order_acquire)) {
            throw;
          }
        }
      }
    };

    std::thread input;
    ScopeGuard join_input([&]() noexcept {
      run->Stop();
      if (input.joinable()) {
        input.join();
      }
    });
    if (!run->inputs.empty()) {
      input = std::thread([run] { run->Pump(); });
    }

    const auto id = state->context->Connection().AllocateInvocation();
    {
      std::lock_guard lock(run->mutex);
      run->invocation.store(id, std::memory_order_release);
    }

    run->changed.notify_all();
    if (request.IsCancellationRequested()) {
      run->Cancel();
    }

    auto result = state->context->Connection().Call(id, Operation::ProcessRequest, payload, std::move(deliver));
    run->Stop();
    if (input.joinable()) {
      input.join();
    }

    if (result.status != FOUNDRY_LOCAL_OK && run->input_error) {
      std::rethrow_exception(run->input_error);
    }

    auto value = Value(std::move(result));
    auto received = DecodeResponse(value.at("response"), {});
    const auto turns = value.at("turns").get<size_t>();
    response = std::move(received);
    state->turns.store(turns, std::memory_order_release);
    request.TryComplete();
  }

 private:
  static Json Payload(const std::shared_ptr<State>& state, Json payload) {
    payload["object"] = EncodeObjectIdentity(state->identity);
    for (auto* scope = callback_scope; scope; scope = scope->previous) {
      if (scope->session == state.get()) {
        payload["callback_parent"] = scope->invocation;
        break;
      }
    }

    return payload;
  }

  static Json Call(const std::shared_ptr<State>& state, Operation operation, Json payload) {
    return state->context->Call(operation, Payload(state, std::move(payload)));
  }

  std::shared_ptr<State> state_;
};

}  // namespace

HostedContext::HostedContext(std::shared_ptr<hosting::SdkConnection> connection, std::string incarnation,
                             uint64_t generation)
    : connection_(std::move(connection)), incarnation_(std::move(incarnation)), generation_(generation) {
  if (!connection_ || incarnation_.empty() || incarnation_.find('\0') != std::string::npos || generation_ == 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid hosted SDK connection");
  }
}

HostedContext::~HostedContext() { Close(); }

hosting::SdkConnection& HostedContext::Connection() const {
  if (closed_.load(std::memory_order_acquire)) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "hosted SDK manager is closed");
  }

  return *connection_;
}

void HostedContext::Validate(const hosting::RemoteObjectIdentity& identity) const {
  static_cast<void>(Connection());
  if (identity.incarnation != incarnation_ || identity.connection_generation != generation_ || identity.object == 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "object does not belong to this hosted SDK connection");
  }
}

void HostedContext::Close() noexcept {
  if (!closed_.exchange(true, std::memory_order_acq_rel)) {
    connection_->Close();
  }
}

void HostedContext::Release(hosting::Operation operation, const hosting::RemoteObjectIdentity& identity) noexcept {
  try {
    Call(operation, {{"object", hosting::EncodeObjectIdentity(identity)}});
  } catch (...) {
    // Disposal cannot reconnect, replay, or unwind a language binding's native finalizer.
  }
}

nlohmann::json HostedContext::Call(hosting::Operation operation, const nlohmann::json& payload) const {
  return Value(Connection().Call(operation, payload));
}

std::unique_ptr<SessionBackend> CreateHostedSession(std::shared_ptr<HostedContext> context,
                                                    hosting::RemoteObjectIdentity identity,
                                                    std::shared_ptr<void> owner) {
  if (!context) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "hosted session requires a manager context");
  }

  return std::make_unique<HostedSession>(std::move(context), std::move(identity), std::move(owner));
}

}  // namespace fl::api
