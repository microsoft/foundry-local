// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <napi.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace foundry_local_node {

class WorkerStartGate {
 public:
  WorkerStartGate(Napi::Env env, Napi::Function callback, const char* resource_name, const char* error_context)
      : state_(std::make_shared<State>()),
        tsfn_(Napi::ThreadSafeFunction::New(env, callback, resource_name, 1, 1)),
        error_context_(error_context) {}

  void SignalAndWait() {
    auto state = state_;
    napi_status status = tsfn_.BlockingCall([state](Napi::Env /*env*/, Napi::Function callback) {
      AcknowledgeOnExit acknowledge{state};
      callback.Call({});
    });
    if (status != napi_ok) {
      Reset(true);
      throw std::runtime_error("Failed to invoke " + error_context_ + " worker-start callback");
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(lock, std::chrono::seconds(10), [state]() { return state->acknowledged; })) {
      lock.unlock();
      Reset(true);
      throw std::runtime_error("Timed out waiting for " + error_context_ + " worker-start callback");
    }
    lock.unlock();
    Reset(false);
  }

 private:
  struct State {
    std::mutex mutex;
    std::condition_variable condition;
    bool acknowledged = false;
  };

  struct AcknowledgeOnExit {
    std::shared_ptr<State> state;

    ~AcknowledgeOnExit() {
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->acknowledged = true;
      }
      state->condition.notify_one();
    }
  };

  void Reset(bool abort) {
    if (abort) {
      tsfn_.Abort();
    } else {
      tsfn_.Release();
    }
    tsfn_ = Napi::ThreadSafeFunction();
  }

  std::shared_ptr<State> state_;
  Napi::ThreadSafeFunction tsfn_;
  std::string error_context_;
};

inline std::shared_ptr<WorkerStartGate> ReadWorkerStartGate(const Napi::CallbackInfo& info, size_t index,
                                                            const char* resource_name, const char* error_context) {
  if (info.Length() <= index || info[index].IsUndefined()) {
    return nullptr;
  }
  if (!info[index].IsFunction()) {
    Napi::TypeError::New(info.Env(), "Internal worker-start hook must be a function").ThrowAsJavaScriptException();
    return nullptr;
  }
  return std::make_shared<WorkerStartGate>(info.Env(), info[index].As<Napi::Function>(), resource_name, error_context);
}

}  // namespace foundry_local_node