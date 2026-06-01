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

#include <memory>
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

void ThrowFoundryLocalError(Napi::Env env, int code, const std::string& msg) {
  Napi::Error err = Napi::Error::New(env, msg);
  Napi::Object value = err.Value();
  value.Set("name", Napi::String::New(env, "FoundryLocalError"));
  value.Set("code", Napi::Number::New(env, code));
  err.ThrowAsJavaScriptException();
}

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
Napi::Value ProcessRequestOn(Napi::Env env, SessT* sess, const Napi::Value& request_arg,
                             Napi::ObjectReference manager_ref) {
  foundry_local::Request* req = UnwrapRequest(env, request_arg);
  if (req == nullptr) return env.Undefined();  // pending exception
  Napi::ObjectReference req_pin = Napi::Reference<Napi::Object>::New(request_arg.As<Napi::Object>(), 1);

  using Result = std::shared_ptr<foundry_local::Response>;
  struct Pins {
    Napi::ObjectReference manager;
    Napi::ObjectReference request;
  };
  auto pins = std::make_shared<Pins>(Pins{std::move(manager_ref), std::move(req_pin)});

  return PromiseWorker<Result>::Run(
      env,
      [sess, req, pins]() -> Result {
        (void)pins;  // keepalive captured by reference count
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
  Napi::ObjectReference manager;
  Napi::ObjectReference request;
  std::shared_ptr<foundry_local::Response> response;
  std::string err_msg;
  int err_code = 0;
  bool tagged = false;
  bool errored = false;
};

void FinalizeStream(Napi::Env env, void* /*data*/, StreamCtx* ctx) {
  Napi::HandleScope scope(env);
  if (ctx->errored) {
    if (ctx->tagged) {
      Napi::Error err = Napi::Error::New(env, ctx->err_msg);
      Napi::Object v = err.Value();
      v.Set("name", Napi::String::New(env, "FoundryLocalError"));
      v.Set("code", Napi::Number::New(env, ctx->err_code));
      ctx->deferred.Reject(v);
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
class StreamWorker : public Napi::AsyncWorker {
 public:
  static Napi::Promise Run(Napi::Env env, SessT* sess, foundry_local::Request* req,
                           Napi::Function jsCallback, StreamCtx* ctx) {
    auto* w = new StreamWorker(env, sess, req, jsCallback, ctx);
    Napi::Promise p = ctx->deferred.Promise();
    w->Queue();
    return p;
  }

  void Execute() override {
    try {
      auto tsfn = tsfn_;
      auto* ctx = ctx_;
      sess_->SetStreamingCallback([tsfn, ctx](flStreamingCallbackData data) -> int {
        (void)ctx;
        if (data.item_queue == nullptr) return 0;
        flItem* raw = nullptr;
        while (foundry_local::detail::item_api()->ItemQueue_TryPop(data.item_queue, &raw)) {
          if (raw == nullptr) break;
          auto* item = new foundry_local::Item(*raw);
          napi_status status = tsfn.BlockingCall(
              item, [](Napi::Env env, Napi::Function jsCb, foundry_local::Item* it) {
                Napi::HandleScope scope(env);
                Napi::Value js_item = ItemToJs(env, *it);
                delete it;
                jsCb.Call({js_item});
              });
          if (status != napi_ok) {
            delete item;
            return 1;
          }
          raw = nullptr;
        }
        return 0;
      });
      ctx_->response = std::make_shared<foundry_local::Response>(sess_->ProcessRequest(*req_));
      // Drop the callback so any stale shared state in the lambda is released
      // before the Session is re-used for a follow-up request.
      sess_->SetStreamingCallback(nullptr);
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
  StreamWorker(Napi::Env env, SessT* sess, foundry_local::Request* req,
               Napi::Function jsCallback, StreamCtx* ctx)
      : Napi::AsyncWorker(env),
        sess_(sess),
        req_(req),
        ctx_(ctx),
        tsfn_(Napi::ThreadSafeFunction::New(env, jsCallback, "foundry_local_stream",
                                            /*max_queue=*/64, /*threads=*/1, ctx,
                                            FinalizeStream,
                                            static_cast<void*>(nullptr))) {}

  SessT* sess_;
  foundry_local::Request* req_;
  StreamCtx* ctx_;
  Napi::ThreadSafeFunction tsfn_;
};

template <typename SessT>
Napi::Value ProcessStreamingRequestOn(Napi::Env env, SessT* sess, const Napi::CallbackInfo& info,
                                      Napi::ObjectReference manager_ref) {
  if (info.Length() < 2 || !info[1].IsFunction()) {
    Napi::TypeError::New(env, "processStreamingRequest(request: Request, onItem: (item) => void)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  foundry_local::Request* req = UnwrapRequest(env, info[0]);
  if (req == nullptr) return env.Undefined();  // pending exception
  Napi::ObjectReference req_pin =
      Napi::Reference<Napi::Object>::New(info[0].As<Napi::Object>(), 1);

  auto* ctx = new StreamCtx{Napi::Promise::Deferred::New(env),
                            std::move(manager_ref),
                            std::move(req_pin),
                            nullptr,
                            "",
                            0,
                            false,
                            false};
  return StreamWorker<SessT>::Run(env, sess, req, info[1].As<Napi::Function>(), ctx);
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
  foundry_local::IModel* native = model != nullptr ? model->native_impl() : nullptr;
  if (native == nullptr) {
    Napi::TypeError::New(env, "ChatSession: Model is not initialized")
        .ThrowAsJavaScriptException();
    return;
  }
  try {
    impl_ = std::make_unique<foundry_local::ChatSession>(*native);
  } catch (const foundry_local::Error& e) {
    ThrowFoundryLocalError(env, static_cast<int>(e.Code()), e.what());
    return;
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return;
  }
  manager_ = Napi::Reference<Napi::Object>::New(model->manager().Value(), 1);
}

bool ChatSession::ThrowIfDisposed(Napi::Env env) {
  if (impl_ == nullptr) {
    ThrowFoundryLocalError(env, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                           "ChatSession has been disposed");
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
  Napi::ObjectReference owner = Napi::Reference<Napi::Object>::New(manager_.Value(), 1);
  return ProcessRequestOn(env, impl_.get(), info[0], std::move(owner));
}

Napi::Value ChatSession::ProcessStreamingRequest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  Napi::ObjectReference owner = Napi::Reference<Napi::Object>::New(manager_.Value(), 1);
  return ProcessStreamingRequestOn(env, impl_.get(), info, std::move(owner));
}

Napi::Value ChatSession::SetOptions(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "setOptions(options: RequestOptions)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object opts = info[0].As<Napi::Object>();
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
        env, "addToolDefinition({ name, description, jsonSchema }: ToolDefinition)")
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
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    foundry_local::ToolDefinition tool(getStr("name"), getStr("description"),
                                       getStr("jsonSchema"));
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
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    bool removed = impl_->RemoveToolDefinition(name);
    return Napi::Boolean::New(env, removed);
  });
}

Napi::Value ChatSession::TurnCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
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
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    impl_->UndoTurns(count);
    return env.Undefined();
  });
}

Napi::Value ChatSession::Dispose(const Napi::CallbackInfo& info) {
  impl_.reset();
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
  foundry_local::IModel* native = model != nullptr ? model->native_impl() : nullptr;
  if (native == nullptr) {
    Napi::TypeError::New(env, "EmbeddingsSession: Model is not initialized")
        .ThrowAsJavaScriptException();
    return;
  }
  try {
    impl_ = std::make_unique<foundry_local::EmbeddingsSession>(*native);
  } catch (const foundry_local::Error& e) {
    ThrowFoundryLocalError(env, static_cast<int>(e.Code()), e.what());
    return;
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return;
  }
  manager_ = Napi::Reference<Napi::Object>::New(model->manager().Value(), 1);
}

bool EmbeddingsSession::ThrowIfDisposed(Napi::Env env) {
  if (impl_ == nullptr) {
    ThrowFoundryLocalError(env, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                           "EmbeddingsSession has been disposed");
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
  return ProcessRequestOn(env, impl_.get(), info[0], std::move(owner));
}

Napi::Value EmbeddingsSession::SetOptions(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "setOptions(options: RequestOptions)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object opts = info[0].As<Napi::Object>();
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    auto request_options = JsToRequestOptions(env, opts);
    impl_->SetOptions(request_options);
    return env.Undefined();
  });
}

Napi::Value EmbeddingsSession::Dispose(const Napi::CallbackInfo& info) {
  impl_.reset();
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
  foundry_local::IModel* native = model != nullptr ? model->native_impl() : nullptr;
  if (native == nullptr) {
    Napi::TypeError::New(env, "AudioSession: Model is not initialized")
        .ThrowAsJavaScriptException();
    return;
  }
  try {
    impl_ = std::make_unique<foundry_local::AudioSession>(*native);
  } catch (const foundry_local::Error& e) {
    ThrowFoundryLocalError(env, static_cast<int>(e.Code()), e.what());
    return;
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return;
  }
  manager_ = Napi::Reference<Napi::Object>::New(model->manager().Value(), 1);
}

bool AudioSession::ThrowIfDisposed(Napi::Env env) {
  if (impl_ == nullptr) {
    ThrowFoundryLocalError(env, FOUNDRY_LOCAL_ERROR_INVALID_USAGE,
                           "AudioSession has been disposed");
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
  return ProcessRequestOn(env, impl_.get(), info[0], std::move(owner));
}

Napi::Value AudioSession::ProcessStreamingRequest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  Napi::ObjectReference owner = Napi::Reference<Napi::Object>::New(manager_.Value(), 1);
  return ProcessStreamingRequestOn(env, impl_.get(), info, std::move(owner));
}

Napi::Value AudioSession::SetOptions(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "setOptions(options: RequestOptions)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object opts = info[0].As<Napi::Object>();
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    auto request_options = JsToRequestOptions(env, opts);
    impl_->SetOptions(request_options);
    return env.Undefined();
  });
}

Napi::Value AudioSession::Dispose(const Napi::CallbackInfo& info) {
  impl_.reset();
  return info.Env().Undefined();
}

Napi::Value AudioSession::IsDisposed(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), impl_ == nullptr);
}

}  // namespace foundry_local_node
