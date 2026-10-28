// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "hosting/session_dispatcher.h"

#include "c_api_types.h"
#include "exception.h"
#include "hosting/sdk_values.h"
#include "inferencing/generative/chat/search_options.h"
#include "util/scope_guard.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <thread>

namespace fl::hosting {
namespace {

using Json = nlohmann::json;
using IncomingCall = SdkConnection::IncomingCall;

SdkResult Failure(std::exception_ptr error) {
  try {
    std::rethrow_exception(error);
  } catch (const Exception& failure) {
    return {static_cast<int32_t>(failure.code()), nullptr, failure.what()};
  } catch (const Json::exception&) {
    return {FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, nullptr, "invalid SDK session payload"};
  } catch (const std::exception& failure) {
    return {FOUNDRY_LOCAL_ERROR_INTERNAL, nullptr, failure.what()};
  } catch (...) {
    return {FOUNDRY_LOCAL_ERROR_INTERNAL, nullptr, "SDK session execution failed"};
  }
}

uint64_t InputNumber(const Json& value) {
  if (!value.is_number_unsigned()) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid SDK session input identity");
  }

  return value.get<uint64_t>();
}

uint64_t InputId(const Json& value) {
  const auto id = InputNumber(value);
  if (id == 0) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "invalid SDK session input identity");
  }

  return id;
}

void RejectCallbackMutation(const IncomingCall& call, const HostObjects::SessionResource& session) {
  const auto found = call.Payload().find("callback_parent");
  if (found != call.Payload().end() && !found->is_null() && session.HasInvocation(InputId(*found))) {
    FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "session mutation is unavailable from its streaming callback");
  }
}

struct RequestRun {
  std::shared_ptr<IncomingCall> call;
  std::shared_ptr<Request> request;
  std::map<uint64_t, ItemQueue*> queues;
  std::atomic<bool> stopping{false};
  std::mutex mutex;
  std::condition_variable changed;
  uint64_t callback_sequence = 0;
  bool acknowledged = false;
  bool callback_cancel = false;
  bool canceled = false;
  bool input_closed = false;
  std::optional<std::string> callback_exception;
  std::exception_ptr input_error;

  void FinishQueues() noexcept {
    for (const auto& [id, queue] : queues) {
      queue->MarkFinished();
    }
  }

  void Cancel() noexcept {
    request->CancelCurrentOrNext();
    {
      std::lock_guard lock(mutex);
      canceled = true;
    }

    FinishQueues();
    changed.notify_all();
  }

  void ReceiveInput() noexcept {
    try {
      while (!stopping.load(std::memory_order_acquire)) {
        const auto input = call->ReadInput();
        if (!input) {
          {
            std::lock_guard lock(mutex);
            input_closed = true;
          }

          FinishQueues();
          changed.notify_all();
          return;
        }

        const auto kind = input->at("kind").get<std::string>();
        if (kind == "callback") {
          const auto sequence = InputId(input->at("sequence"));
          const auto cancel = input->at("cancel").get<bool>();
          std::optional<std::string> exception;
          if (!input->at("exception").is_null()) {
            exception = input->at("exception").get<std::string>();
          }

          {
            std::lock_guard lock(mutex);
            if (sequence != callback_sequence || acknowledged) {
              FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unexpected SDK streaming acknowledgment");
            }

            acknowledged = true;
            callback_cancel = cancel;
            callback_exception = std::move(exception);
          }

          changed.notify_all();
        } else if (kind == "item" || kind == "finish_queue") {
          const auto found = queues.find(InputId(input->at("queue")));
          if (found == queues.end() || found->second->IsFinished()) {
            FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unknown or finished SDK input queue");
          }

          if (kind == "item") {
            found->second->Push(DecodeItem(input->at("item"), {}));
          } else {
            found->second->MarkFinished();
          }
        } else {
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "unknown SDK session input");
        }
      }
    } catch (...) {
      if (!stopping.load(std::memory_order_acquire) && !call->IsCancelled()) {
        std::lock_guard lock(mutex);
        input_error = std::current_exception();
      }

      Cancel();
    }
  }

  static int Stream(flStreamingCallbackData data, void* context) {
    auto& run = *static_cast<RequestRun*>(context);
    Json items = Json::array();
    auto* queue = AsImpl(data.item_queue);
    while (auto item = queue->TryPop()) {
      items.push_back(EncodeItem(*item, {}));
    }

    uint64_t sequence;
    {
      std::lock_guard lock(run.mutex);
      if (run.input_closed || run.canceled) {
        run.request->CancelCurrentOrNext();
        return 1;
      }

      sequence = ++run.callback_sequence;
      run.acknowledged = false;
      run.callback_cancel = false;
      run.callback_exception.reset();
    }

    run.call->SendEvent({{"kind", "stream"}, {"sequence", sequence}, {"items", std::move(items)}});
    std::unique_lock lock(run.mutex);
    run.changed.wait(lock, [&] { return run.acknowledged || run.canceled || run.input_closed; });
    if (run.callback_exception) {
      throw std::runtime_error(*run.callback_exception);
    }

    return run.canceled || run.input_closed || run.callback_cancel ? 1 : 0;
  }
};

}  // namespace

bool SessionDispatcher::Handles(Operation operation) noexcept {
  switch (operation) {
    case Operation::ReleaseSession:
    case Operation::SessionOptions:
    case Operation::AddTool:
    case Operation::RemoveTool:
    case Operation::UndoTurns:
    case Operation::CapturePreflight:
    case Operation::ExecutePreflight:
    case Operation::ReleasePreflight:
    case Operation::ProcessRequest:
      return true;
    default:
      return false;
  }
}

void SessionDispatcher::Dispatch(const std::shared_ptr<IncomingCall>& call) {
  SdkResult result;
  try {
    if (call->IsCancelled()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "SDK invocation cancelled before execution");
    }

    const auto identity = DecodeObjectIdentity(call->Payload().at("object"));
    if (call->Op() == Operation::ReleaseSession) {
      objects_.ReleaseSession(identity);
    } else if (call->Op() == Operation::ReleasePreflight) {
      objects_.ReleasePreflight(identity);
    } else if (call->Op() == Operation::ExecutePreflight) {
      const auto preflight = objects_.Preflight(identity);
      result.value = EncodeBudget(preflight->implementation->Execute());
    } else {
      const auto session = objects_.Session(identity);
      switch (call->Op()) {
        case Operation::SessionOptions:
          RejectCallbackMutation(*call, *session);
          session->implementation->SetSessionOptions(DecodeOptions(call->Payload().at("options")));
          break;
        case Operation::AddTool:
          session->implementation->AddToolDefinition(DecodeToolDefinition(call->Payload().at("tool")));
          break;
        case Operation::RemoveTool:
          result.value = session->implementation->RemoveToolDefinition(call->Payload().at("name").get<std::string>());
          break;
        case Operation::UndoTurns:
          RejectCallbackMutation(*call, *session);
          session->implementation->UndoTurns(InputNumber(call->Payload().at("count")));
          result.value = session->implementation->TurnCount();
          break;
        case Operation::CapturePreflight: {
          RejectCallbackMutation(*call, *session);
          const auto request = DecodeRequest(call->Payload().at("request"), {});
          auto preflight = session->implementation->CreateRequestPreflight(request);
          result.value = EncodeObjectIdentity(objects_.AddPreflight(std::move(preflight)));
          break;
        }
        case Operation::ProcessRequest:
          Process(call, session);
          return;
        default:
          FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, "operation is not an SDK session operation");
      }
    }
  } catch (...) {
    result = Failure(std::current_exception());
  }

  call->Complete(std::move(result));
}

void SessionDispatcher::Process(const std::shared_ptr<IncomingCall>& call,
                                const std::shared_ptr<HostObjects::SessionResource>& session) {
  RejectCallbackMutation(*call, *session);
  auto run = std::make_shared<RequestRun>();
  run->call = call;
  run->request = std::make_shared<Request>(
      DecodeRequest(call->Payload().at("request"), {
                                                       .open_queue =
                                                           [&](uint64_t id) {
                                                             auto queue = std::make_unique<ItemQueue>();
                                                             if (!run->queues.emplace(id, queue.get()).second) {
                                                               FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT,
                                                                        "duplicate SDK input queue");
                                                             }

                                                             return queue;
                                                           },
                                                   }));
  const bool streaming = call->Payload().at("streaming").get<bool>();
  call->SetCancelHandler([weak = std::weak_ptr(run)] {
    if (const auto active = weak.lock()) {
      active->Cancel();
    }
  });
  session->AdmitInvocation(call->Id(), [weak = std::weak_ptr(run)] {
    if (const auto active = weak.lock()) {
      active->Cancel();
    }
  });
  ScopeGuard clear_invocation([&]() noexcept { session->CompleteInvocation(call->Id()); });

  std::thread input;
  SdkResult result;
  try {
    if (streaming || !run->queues.empty()) {
      input = std::thread([run] { run->ReceiveInput(); });
    }
    if (call->IsCancelled() || session->IsRetired()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "SDK invocation cancelled before execution");
    }

    Response response;
    session->implementation->ProcessRequest(*run->request, response,
                                            StreamingCallback{streaming ? RequestRun::Stream : nullptr, run.get()});
    {
      std::lock_guard lock(run->mutex);
      if (run->input_error) {
        std::rethrow_exception(run->input_error);
      }
    }

    result.value = {{"response", EncodeResponse(response, {})}, {"turns", session->implementation->TurnCount()}};
  } catch (...) {
    auto error = std::current_exception();
    {
      std::lock_guard lock(run->mutex);
      if (run->input_error) {
        error = run->input_error;
      }
    }

    result = Failure(error);
  }

  run->stopping.store(true, std::memory_order_release);
  run->FinishQueues();
  try {
    call->Complete(std::move(result));
  } catch (...) {
    connection_.Close();
  }

  if (input.joinable()) {
    input.join();
  }
}

}  // namespace fl::hosting
