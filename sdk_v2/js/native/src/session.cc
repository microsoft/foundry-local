// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "session.h"

#include "addon_data.h"
#include "errors.h"
#include "items.h"
#include "model.h"
#include "promise_worker.h"
#include "request.h"
#include "request_options.h"

#include <foundry_local/foundry_local_c.h>
#include <foundry_local/foundry_local_cpp.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace foundry_local_node {

namespace {

const char* FinishReasonToString(flFinishReason r) {
  switch (r) {
    case FOUNDRY_LOCAL_FINISH_STOP:
      return "stop";
    case FOUNDRY_LOCAL_FINISH_LENGTH:
      return "length";
    case FOUNDRY_LOCAL_FINISH_TOOL_CALLS:
      return "toolCalls";
    case FOUNDRY_LOCAL_FINISH_ERROR:
      return "error";
    case FOUNDRY_LOCAL_FINISH_NONE:
    default:
      return "none";
  }
}

// Snapshot a Response into a plain JS object so the JS surface has no native
// backing — safe to consume after the worker job completes.
Napi::Value ResponseToJs(Napi::Env env, foundry_local::Response& resp) {
  Napi::Object out = Napi::Object::New(env);
  const auto& items = resp.GetItems();
  Napi::Array arr = Napi::Array::New(env, items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    arr.Set(static_cast<uint32_t>(i), ItemToJs(env, items[i]));
  }
  out.Set("output", arr);
  out.Set("finishReason", Napi::String::New(env, FinishReasonToString(resp.GetFinishReason())));

  flUsage usage = resp.GetUsage();
  Napi::Object u = Napi::Object::New(env);
  u.Set("promptTokens", Napi::Number::New(env, static_cast<double>(usage.prompt_tokens)));
  u.Set("completionTokens", Napi::Number::New(env, static_cast<double>(usage.completion_tokens)));
  u.Set("totalTokens", Napi::Number::New(env, static_cast<double>(usage.total_tokens)));
  out.Set("usage", u);
  return out;
}

class SessionOperationLease {
 public:
  explicit SessionOperationLease(std::shared_ptr<SessionOperationState> state) : state_(std::move(state)) {}
  ~SessionOperationLease() { Release(); }

  void Release() {
    if (!released_.exchange(true)) state_->busy.store(false);
  }

 private:
  std::shared_ptr<SessionOperationState> state_;
  std::atomic_bool released_ = false;
};

class ReleaseSessionOperation {
 public:
  explicit ReleaseSessionOperation(std::shared_ptr<SessionOperationLease> lease) : lease_(std::move(lease)) {}
  ~ReleaseSessionOperation() { lease_->Release(); }

 private:
  std::shared_ptr<SessionOperationLease> lease_;
};

std::shared_ptr<SessionOperationLease> AcquireSessionOperation(Napi::Env env,
                                                               std::shared_ptr<SessionOperationState> state) {
  bool expected = false;
  if (!state->busy.compare_exchange_strong(expected, true)) {
    ThrowFoundryLocalError(env, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                           "Session already has an active operation");
    return nullptr;
  }
  return std::make_shared<SessionOperationLease>(std::move(state));
}

class SessionWorkerGate {
 public:
  SessionWorkerGate(Napi::Env env, Napi::Function callback)
      : state_(std::make_shared<State>()),
        tsfn_(Napi::ThreadSafeFunction::New(env, callback, "Session.processRequest.workerStarted", 1, 1)) {}

  void SignalAndWait() {
    auto state = state_;
    napi_status status = tsfn_.BlockingCall([state](Napi::Env env, Napi::Function callback) {
      callback.Call({Napi::Function::New(env, [state](const Napi::CallbackInfo& info) {
        {
          std::lock_guard<std::mutex> lock(state->mutex);
          state->released = true;
        }
        state->condition.notify_one();
        return info.Env().Undefined();
      })});
    });
    if (status != napi_ok) {
      tsfn_.Abort();
      tsfn_ = Napi::ThreadSafeFunction();
      throw std::runtime_error("Failed to invoke session worker-start callback");
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(lock, std::chrono::seconds(30), [state]() { return state->released; })) {
      lock.unlock();
      tsfn_.Abort();
      tsfn_ = Napi::ThreadSafeFunction();
      throw std::runtime_error("Timed out waiting for session worker-start release");
    }
    lock.unlock();
    tsfn_.Release();
    tsfn_ = Napi::ThreadSafeFunction();
  }

 private:
  struct State {
    std::mutex mutex;
    std::condition_variable condition;
    bool released = false;
  };

  std::shared_ptr<State> state_;
  Napi::ThreadSafeFunction tsfn_;
};

foundry_local::Request* UnwrapRequest(Napi::Env env, const Napi::Value& v) {
  if (!v.IsObject()) {
    Napi::TypeError::New(env, "processRequest(request): expected a Request instance")
        .ThrowAsJavaScriptException();
    return nullptr;
  }
  Napi::Object obj = v.As<Napi::Object>();
  auto* data = env.GetInstanceData<AddonData>();
  if (data == nullptr) {
    Napi::Error::New(env, "Addon data unavailable").ThrowAsJavaScriptException();
    return nullptr;
  }
  if (!obj.InstanceOf(data->request_ctor.Value())) {
    Napi::TypeError::New(env, "processRequest(request): argument is not a Request instance")
        .ThrowAsJavaScriptException();
    return nullptr;
  }
  Request* req = Napi::ObjectWrap<Request>::Unwrap(obj);
  if (req == nullptr || req->native() == nullptr) {
    Napi::TypeError::New(env, "processRequest(request): Request is not initialized")
        .ThrowAsJavaScriptException();
    return nullptr;
  }
  return req->native();
}

// Process a Request on the worker thread, converting to JS in the resolver.
// Pins both the Manager (so the Model handle the Session holds stays alive)
// and the Request (so the C++ Request the worker reads stays alive).
template <typename SessT>
Napi::Value ProcessRequestOn(Napi::Env env, std::shared_ptr<SessT> sess, const Napi::Value& request_arg,
                             std::shared_ptr<foundry_local::Manager> manager_lifetime,
                             Napi::ObjectReference manager_ref,
                             std::shared_ptr<SessionOperationState> operation_state,
                             Napi::Function worker_started = Napi::Function()) {
  foundry_local::Request* req = UnwrapRequest(env, request_arg);
  if (req == nullptr) return env.Undefined();  // pending exception
  auto operation = AcquireSessionOperation(env, std::move(operation_state));
  if (operation == nullptr) return env.Undefined();
  Napi::ObjectReference req_pin = Napi::Reference<Napi::Object>::New(request_arg.As<Napi::Object>(), 1);
  auto worker_gate = worker_started.IsEmpty() ? nullptr : std::make_shared<SessionWorkerGate>(env, worker_started);

  using Result = std::shared_ptr<foundry_local::Response>;
  struct Pins {
    std::shared_ptr<foundry_local::Manager> manager_lifetime;
    Napi::ObjectReference manager;
    Napi::ObjectReference request;
  };
  auto pins = std::make_shared<Pins>(
      Pins{std::move(manager_lifetime), std::move(manager_ref), std::move(req_pin)});

  return PromiseWorker<Result>::Run(
      env,
      [sess, req, pins, operation, worker_gate]() -> Result {
        ReleaseSessionOperation release(operation);
        (void)pins;  // keepalive captured by reference count
        if (worker_gate != nullptr) worker_gate->SignalAndWait();
        return std::make_shared<foundry_local::Response>(sess->ProcessRequest(*req));
      },
      [](Napi::Env env, Result& resp) -> Napi::Value { return ResponseToJs(env, *resp); });
}

// ──────────────────────────────────────────────────────────────────────────
// Streaming bridge
// ──────────────────────────────────────────────────────────────────────────
//
// Native ProcessRequest runs on a libuv worker thread (Napi::AsyncWorker).
// During ProcessRequest the C++ wrapper invokes our std::function streaming
// callback synchronously from that same worker thread for each output item.
// The lambda pops items off the C-API flItemQueue and forwards them to JS
// via a Napi::ThreadSafeFunction.
//
// Backpressure: the TSFN is created with maxQueueSize=64. BlockingCall blocks
// the producer worker thread when the JS-side iterator hasn't drained yet,
// providing classic flow control without dropping items. 64 chosen as a small
// power of two — large enough to hide ~half a second of token-emission jitter
// at modest decoding rates, small enough that a stalled consumer doesn't pin
// hundreds of token-sized heap allocations.
//
// Promise resolution: deliberately deferred to the TSFN's finalize callback
// (which runs on the JS thread AFTER all queued item callbacks drain) so the
// returned Promise never resolves before the consumer has been delivered
// every item the native side produced. Worker OnOK/OnError only release the
// TSFN; they never touch the Deferred.

namespace {

struct StreamCtx {
  Napi::Promise::Deferred deferred;
  std::shared_ptr<foundry_local::Manager> manager_lifetime;
  Napi::ObjectReference manager;
  Napi::ObjectReference request;
  std::shared_ptr<foundry_local::Response> response;
  std::shared_ptr<SessionOperationLease> operation;
  std::string err_msg;
  int err_code = 0;
  bool tagged = false;
  bool errored = false;
};

void FinalizeStream(Napi::Env env, void* /*data*/, StreamCtx* ctx) {
  Napi::HandleScope scope(env);
  if (ctx->errored) {
    if (ctx->tagged) {
      ctx->deferred.Reject(MakeFoundryLocalError(env, ctx->err_code, ctx->err_msg).Value());
    } else {
      ctx->deferred.Reject(Napi::Error::New(env, ctx->err_msg).Value());
    }
  } else if (ctx->response != nullptr) {
    ctx->deferred.Resolve(ResponseToJs(env, *ctx->response));
  } else {
    // Should not happen: successful path always captures a Response. Guard
    // anyway so we never leave the deferred pending.
    ctx->deferred.Resolve(env.Undefined());
  }
  delete ctx;
}

template <typename SessT>
class StreamingCallbackReset {
 public:
  explicit StreamingCallbackReset(std::shared_ptr<SessT> session) : session_(std::move(session)) {}

  ~StreamingCallbackReset() {
    try {
      session_->SetStreamingCallback(nullptr);
    } catch (...) {
      // Cleanup cannot replace the inference result while unwinding.
    }
  }

 private:
  std::shared_ptr<SessT> session_;
};

template <typename SessT>
class StreamWorker : public Napi::AsyncWorker {
 public:
  static Napi::Promise Run(Napi::Env env, std::shared_ptr<SessT> sess, foundry_local::Request* req,
                           Napi::Function jsCallback, StreamCtx* ctx) {
    auto* w = new StreamWorker(env, std::move(sess), req, jsCallback, ctx);
    Napi::Promise p = ctx->deferred.Promise();
    w->Queue();
    return p;
  }

  void Execute() override {
    ReleaseSessionOperation release(operation_);
    try {
      StreamingCallbackReset<SessT> clear_streaming_callback(sess_);
      auto tsfn = tsfn_;
      auto* ctx = ctx_;
      sess_->SetStreamingCallback([tsfn, ctx](foundry_local::Item item) -> int {
        (void)ctx;
        auto* streamed_item = new foundry_local::Item(std::move(item));
        napi_status status = tsfn.BlockingCall(
            streamed_item, [](Napi::Env env, Napi::Function jsCb, foundry_local::Item* item) {
              Napi::HandleScope scope(env);
              Napi::Value js_item = ItemToJs(env, *item);
              delete item;
              jsCb.Call({js_item});
            });
        if (status != napi_ok) {
          delete streamed_item;
          return 1;
        }
        return 0;
      });
      ctx_->response = std::make_shared<foundry_local::Response>(sess_->ProcessRequest(*req_));
    } catch (const foundry_local::Error& e) {
      ctx_->errored = true;
      ctx_->err_code = static_cast<int>(e.Code());
      ctx_->err_msg = e.what();
      ctx_->tagged = true;
    } catch (const std::exception& e) {
      ctx_->errored = true;
      ctx_->err_msg = e.what();
    } catch (...) {
      ctx_->errored = true;
      ctx_->err_msg = "Unknown native exception";
    }
  }

  // Promise resolution happens in FinalizeStream — overriding OnOK/OnError
  // here only releases the TSFN so its finalizer can run on the JS thread
  // once all queued item callbacks have drained.
  void OnOK() override { tsfn_.Release(); }
  void OnError(const Napi::Error& /*unused*/) override { tsfn_.Release(); }

 private:
  StreamWorker(Napi::Env env, std::shared_ptr<SessT> sess, foundry_local::Request* req,
               Napi::Function jsCallback, StreamCtx* ctx)
      : Napi::AsyncWorker(env),
        sess_(std::move(sess)),
        req_(req),
        ctx_(ctx),
        operation_(ctx->operation),
        tsfn_(Napi::ThreadSafeFunction::New(env, jsCallback, "foundry_local_stream",
                                            /*max_queue=*/64, /*threads=*/1, ctx,
                                            FinalizeStream,
                                            static_cast<void*>(nullptr))) {}

  std::shared_ptr<SessT> sess_;
  foundry_local::Request* req_;
  StreamCtx* ctx_;
  std::shared_ptr<SessionOperationLease> operation_;
  Napi::ThreadSafeFunction tsfn_;
};

template <typename SessT>
Napi::Value ProcessStreamingRequestOn(Napi::Env env, std::shared_ptr<SessT> sess, const Napi::CallbackInfo& info,
                                      std::shared_ptr<foundry_local::Manager> manager_lifetime,
                                      Napi::ObjectReference manager_ref,
                                      std::shared_ptr<SessionOperationState> operation_state) {
  if (info.Length() < 2 || !info[1].IsFunction()) {
    Napi::TypeError::New(env, "processStreamingRequest(request: Request, onItem: (item) => void)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  foundry_local::Request* req = UnwrapRequest(env, info[0]);
  if (req == nullptr) return env.Undefined();  // pending exception
  auto operation = AcquireSessionOperation(env, std::move(operation_state));
  if (operation == nullptr) return env.Undefined();
  Napi::ObjectReference req_pin =
      Napi::Reference<Napi::Object>::New(info[0].As<Napi::Object>(), 1);

  auto* ctx = new StreamCtx{Napi::Promise::Deferred::New(env),
                            std::move(manager_lifetime),
                            std::move(manager_ref),
                            std::move(req_pin),
                            nullptr,
                            std::move(operation),
                            "",
                            0,
                            false,
                            false};
  return StreamWorker<SessT>::Run(env, std::move(sess), req, info[1].As<Napi::Function>(), ctx);
}

}  // namespace

}  // namespace

// ──────────────────────────────────────────────────────────────────────────
// ChatSession
// ──────────────────────────────────────────────────────────────────────────

Napi::Function ChatSession::Init(Napi::Env env) {
  return DefineClass(env, "ChatSession",
                     {
                         InstanceMethod("processRequest", &ChatSession::ProcessRequest),
                         InstanceMethod("processStreamingRequest", &ChatSession::ProcessStreamingRequest),
                         InstanceMethod("setOptions", &ChatSession::SetOptions),
                         InstanceMethod("addToolDefinition", &ChatSession::AddToolDefinition),
                         InstanceMethod("removeToolDefinition", &ChatSession::RemoveToolDefinition),
                         InstanceMethod("turnCount", &ChatSession::TurnCount),
                         InstanceMethod("undoTurns", &ChatSession::UndoTurns),
                         InstanceMethod("dispose", &ChatSession::Dispose),
                         InstanceMethod("isDisposed", &ChatSession::IsDisposed),
                     });
}

ChatSession::ChatSession(const Napi::CallbackInfo& info) : Napi::ObjectWrap<ChatSession>(info) {
  Napi::Env env = info.Env();
  auto* data = env.GetInstanceData<AddonData>();
  if (info.Length() != 1 || !info[0].IsObject() ||
      data == nullptr ||
      !info[0].As<Napi::Object>().InstanceOf(data->model_ctor.Value())) {
    Napi::TypeError::New(env, "ChatSession: expected a Model as the first argument")
        .ThrowAsJavaScriptException();
    return;
  }
  Napi::Object model_obj = info[0].As<Napi::Object>();
  Model* model = Napi::ObjectWrap<Model>::Unwrap(model_obj);
  foundry_local::IModel* native = model != nullptr ? model->native_impl(env) : nullptr;
  if (native == nullptr) {
    Napi::TypeError::New(env, "ChatSession: Model is not initialized")
        .ThrowAsJavaScriptException();
    return;
  }
  auto manager_lifetime = model->LockManager(env);
  if (manager_lifetime == nullptr) return;
  try {
    impl_ = std::make_shared<foundry_local::ChatSession>(*native);
  } catch (const foundry_local::Error& e) {
    ThrowFoundryLocalError(env, static_cast<int>(e.Code()), e.what());
    return;
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return;
  }
  manager_lifetime_ = std::move(manager_lifetime);
  manager_disposed_ = model->disposed_state();
  manager_ = Napi::Reference<Napi::Object>::New(model->manager().Value(), 1);
}

bool ChatSession::ThrowIfDisposed(Napi::Env env) {
  if (impl_ == nullptr) {
    ThrowFoundryLocalError(env, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                           "ChatSession has been disposed");
    return true;
  }
  if (manager_disposed_->load()) {
    ThrowFoundryLocalError(env, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Manager has been disposed");
    return true;
  }
  return false;
}

Napi::Value ChatSession::ProcessRequest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  if (info.Length() < 1) {
    Napi::TypeError::New(env, "processRequest(request: Request)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Function worker_started;
  if (info.Length() >= 2 && !info[1].IsUndefined()) {
    if (!info[1].IsFunction()) {
      Napi::TypeError::New(env, "Internal worker-start hook must be a function").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    worker_started = info[1].As<Napi::Function>();
  }
  Napi::ObjectReference owner = Napi::Reference<Napi::Object>::New(manager_.Value(), 1);
  return ProcessRequestOn(env, impl_, info[0], manager_lifetime_, std::move(owner), operation_state_, worker_started);
}

Napi::Value ChatSession::ProcessStreamingRequest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  Napi::ObjectReference owner = Napi::Reference<Napi::Object>::New(manager_.Value(), 1);
  return ProcessStreamingRequestOn(env, impl_, info, manager_lifetime_, std::move(owner), operation_state_);
}

Napi::Value ChatSession::SetOptions(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "setOptions(options: RequestOptions)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object opts = info[0].As<Napi::Object>();
  auto operation = AcquireSessionOperation(env, operation_state_);
  if (operation == nullptr) return env.Undefined();
  ReleaseSessionOperation release(operation);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    auto request_options = JsToRequestOptions(env, opts);
    impl_->SetOptions(request_options);
    return env.Undefined();
  });
}

Napi::Value ChatSession::AddToolDefinition(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(
        env, "addToolDefinition({ name, description, jsonSchema, kind? }: ToolDefinition)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object def = info[0].As<Napi::Object>();
  auto getStr = [&](const char* key) -> std::string {
    if (!def.Has(key) || !def.Get(key).IsString()) {
      throw Napi::TypeError::New(env, std::string("addToolDefinition: '") + key +
                                          "' must be a string");
    }
    return def.Get(key).As<Napi::String>().Utf8Value();
  };

  // `kind` is optional, so the pre-existing three-property shape keeps working unchanged. It is
  // validated here rather than inside CallChecked so a bad shape surfaces as a TypeError, like the
  // other argument-shape checks, instead of a native FoundryLocalError.
  flToolKind kind = FOUNDRY_LOCAL_TOOL_KIND_FUNCTION;
  if (def.Has("kind") && !def.Get("kind").IsUndefined()) {
    std::string kind_name =
        def.Get("kind").IsString() ? def.Get("kind").As<Napi::String>().Utf8Value() : std::string{};
    if (kind_name == "custom") {
      kind = FOUNDRY_LOCAL_TOOL_KIND_CUSTOM;
    } else if (kind_name != "function") {
      Napi::TypeError::New(env, "addToolDefinition: 'kind' must be 'function' or 'custom'")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
  }

  auto operation = AcquireSessionOperation(env, operation_state_);
  if (operation == nullptr) return env.Undefined();
  ReleaseSessionOperation release(operation);

  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    // `jsonSchema` is required for a function tool and omitted for a custom one, which carries no
    // schema of its own. A custom tool that does supply one is rejected natively.
    std::string json_schema;
    if (kind == FOUNDRY_LOCAL_TOOL_KIND_FUNCTION || def.Has("jsonSchema")) {
      json_schema = getStr("jsonSchema");
    }

    foundry_local::ToolDefinition tool(getStr("name"), getStr("description"),
                                       std::move(json_schema));
    tool.kind = kind;
    impl_->AddToolDefinition(tool);
    return env.Undefined();
  });
}

Napi::Value ChatSession::RemoveToolDefinition(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "removeToolDefinition(name: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string name = info[0].As<Napi::String>().Utf8Value();
  auto operation = AcquireSessionOperation(env, operation_state_);
  if (operation == nullptr) return env.Undefined();
  ReleaseSessionOperation release(operation);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    bool removed = impl_->RemoveToolDefinition(name);
    return Napi::Boolean::New(env, removed);
  });
}

Napi::Value ChatSession::TurnCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  auto operation = AcquireSessionOperation(env, operation_state_);
  if (operation == nullptr) return env.Undefined();
  ReleaseSessionOperation release(operation);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    return Napi::Number::New(env, static_cast<double>(impl_->TurnCount()));
  });
}

Napi::Value ChatSession::UndoTurns(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "undoTurns(count: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  size_t count = static_cast<size_t>(info[0].As<Napi::Number>().Uint32Value());
  auto operation = AcquireSessionOperation(env, operation_state_);
  if (operation == nullptr) return env.Undefined();
  ReleaseSessionOperation release(operation);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    impl_->UndoTurns(count);
    return env.Undefined();
  });
}

Napi::Value ChatSession::Dispose(const Napi::CallbackInfo& info) {
  impl_.reset();
  manager_lifetime_.reset();
  manager_.Reset();
  return info.Env().Undefined();
}

Napi::Value ChatSession::IsDisposed(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), impl_ == nullptr);
}

// ──────────────────────────────────────────────────────────────────────────
// EmbeddingsSession
// ──────────────────────────────────────────────────────────────────────────

Napi::Function EmbeddingsSession::Init(Napi::Env env) {
  return DefineClass(env, "EmbeddingsSession",
                     {
                         InstanceMethod("processRequest", &EmbeddingsSession::ProcessRequest),
                         InstanceMethod("setOptions", &EmbeddingsSession::SetOptions),
                         InstanceMethod("dispose", &EmbeddingsSession::Dispose),
                         InstanceMethod("isDisposed", &EmbeddingsSession::IsDisposed),
                     });
}

EmbeddingsSession::EmbeddingsSession(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<EmbeddingsSession>(info) {
  Napi::Env env = info.Env();
  auto* data = env.GetInstanceData<AddonData>();
  if (info.Length() != 1 || !info[0].IsObject() ||
      data == nullptr ||
      !info[0].As<Napi::Object>().InstanceOf(data->model_ctor.Value())) {
    Napi::TypeError::New(env, "EmbeddingsSession: expected a Model as the first argument")
        .ThrowAsJavaScriptException();
    return;
  }
  Napi::Object model_obj = info[0].As<Napi::Object>();
  Model* model = Napi::ObjectWrap<Model>::Unwrap(model_obj);
  foundry_local::IModel* native = model != nullptr ? model->native_impl(env) : nullptr;
  if (native == nullptr) {
    Napi::TypeError::New(env, "EmbeddingsSession: Model is not initialized")
        .ThrowAsJavaScriptException();
    return;
  }
  auto manager_lifetime = model->LockManager(env);
  if (manager_lifetime == nullptr) return;
  try {
    impl_ = std::make_shared<foundry_local::EmbeddingsSession>(*native);
  } catch (const foundry_local::Error& e) {
    ThrowFoundryLocalError(env, static_cast<int>(e.Code()), e.what());
    return;
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return;
  }
  manager_lifetime_ = std::move(manager_lifetime);
  manager_disposed_ = model->disposed_state();
  manager_ = Napi::Reference<Napi::Object>::New(model->manager().Value(), 1);
}

bool EmbeddingsSession::ThrowIfDisposed(Napi::Env env) {
  if (impl_ == nullptr) {
    ThrowFoundryLocalError(env, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                           "EmbeddingsSession has been disposed");
    return true;
  }
  if (manager_disposed_->load()) {
    ThrowFoundryLocalError(env, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Manager has been disposed");
    return true;
  }
  return false;
}

Napi::Value EmbeddingsSession::ProcessRequest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  if (info.Length() < 1) {
    Napi::TypeError::New(env, "processRequest(request: Request)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::ObjectReference owner = Napi::Reference<Napi::Object>::New(manager_.Value(), 1);
  return ProcessRequestOn(env, impl_, info[0], manager_lifetime_, std::move(owner), operation_state_);
}

Napi::Value EmbeddingsSession::SetOptions(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "setOptions(options: RequestOptions)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object opts = info[0].As<Napi::Object>();
  auto operation = AcquireSessionOperation(env, operation_state_);
  if (operation == nullptr) return env.Undefined();
  ReleaseSessionOperation release(operation);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    auto request_options = JsToRequestOptions(env, opts);
    impl_->SetOptions(request_options);
    return env.Undefined();
  });
}

Napi::Value EmbeddingsSession::Dispose(const Napi::CallbackInfo& info) {
  impl_.reset();
  manager_lifetime_.reset();
  manager_.Reset();
  return info.Env().Undefined();
}

Napi::Value EmbeddingsSession::IsDisposed(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), impl_ == nullptr);
}

// ───────────────────────────────────────────────────────────────────
// AudioSession
// ───────────────────────────────────────────────────────────────────
//
// Mirrors EmbeddingsSession but additionally registers processStreamingRequest
// because audio transcription supports incremental output (and live
// transcription drives streaming input through an ItemQueue).

Napi::Function AudioSession::Init(Napi::Env env) {
  return DefineClass(env, "AudioSession",
                     {
                         InstanceMethod("processRequest", &AudioSession::ProcessRequest),
                         InstanceMethod("processStreamingRequest", &AudioSession::ProcessStreamingRequest),
                         InstanceMethod("setOptions", &AudioSession::SetOptions),
                         InstanceMethod("dispose", &AudioSession::Dispose),
                         InstanceMethod("isDisposed", &AudioSession::IsDisposed),
                     });
}

AudioSession::AudioSession(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<AudioSession>(info) {
  Napi::Env env = info.Env();
  auto* data = env.GetInstanceData<AddonData>();
  if (info.Length() != 1 || !info[0].IsObject() ||
      data == nullptr ||
      !info[0].As<Napi::Object>().InstanceOf(data->model_ctor.Value())) {
    Napi::TypeError::New(env, "AudioSession: expected a Model as the first argument")
        .ThrowAsJavaScriptException();
    return;
  }
  Napi::Object model_obj = info[0].As<Napi::Object>();
  Model* model = Napi::ObjectWrap<Model>::Unwrap(model_obj);
  foundry_local::IModel* native = model != nullptr ? model->native_impl(env) : nullptr;
  if (native == nullptr) {
    Napi::TypeError::New(env, "AudioSession: Model is not initialized")
        .ThrowAsJavaScriptException();
    return;
  }
  auto manager_lifetime = model->LockManager(env);
  if (manager_lifetime == nullptr) return;
  try {
    impl_ = std::make_shared<foundry_local::AudioSession>(*native);
  } catch (const foundry_local::Error& e) {
    ThrowFoundryLocalError(env, static_cast<int>(e.Code()), e.what());
    return;
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return;
  }
  manager_lifetime_ = std::move(manager_lifetime);
  manager_disposed_ = model->disposed_state();
  manager_ = Napi::Reference<Napi::Object>::New(model->manager().Value(), 1);
}

bool AudioSession::ThrowIfDisposed(Napi::Env env) {
  if (impl_ == nullptr) {
    ThrowFoundryLocalError(env, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                           "AudioSession has been disposed");
    return true;
  }
  if (manager_disposed_->load()) {
    ThrowFoundryLocalError(env, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Manager has been disposed");
    return true;
  }
  return false;
}

Napi::Value AudioSession::ProcessRequest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  if (info.Length() < 1) {
    Napi::TypeError::New(env, "processRequest(request: Request)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::ObjectReference owner = Napi::Reference<Napi::Object>::New(manager_.Value(), 1);
  return ProcessRequestOn(env, impl_, info[0], manager_lifetime_, std::move(owner), operation_state_);
}

Napi::Value AudioSession::ProcessStreamingRequest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  Napi::ObjectReference owner = Napi::Reference<Napi::Object>::New(manager_.Value(), 1);
  return ProcessStreamingRequestOn(env, impl_, info, manager_lifetime_, std::move(owner), operation_state_);
}

Napi::Value AudioSession::SetOptions(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "setOptions(options: RequestOptions)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object opts = info[0].As<Napi::Object>();
  auto operation = AcquireSessionOperation(env, operation_state_);
  if (operation == nullptr) return env.Undefined();
  ReleaseSessionOperation release(operation);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    auto request_options = JsToRequestOptions(env, opts);
    impl_->SetOptions(request_options);
    return env.Undefined();
  });
}

Napi::Value AudioSession::Dispose(const Napi::CallbackInfo& info) {
  impl_.reset();
  manager_lifetime_.reset();
  manager_.Reset();
  return info.Env().Undefined();
}

Napi::Value AudioSession::IsDisposed(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), impl_ == nullptr);
}

}  // namespace foundry_local_node
