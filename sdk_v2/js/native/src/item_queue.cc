// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "item_queue.h"

#include "errors.h"
#include "items.h"

#include <foundry_local/foundry_local_c.h>
#include <foundry_local/foundry_local_cpp.h>

#include <memory>
#include <optional>
#include <utility>

namespace foundry_local_node {

Napi::Function NativeItemQueue::Init(Napi::Env env) {
  return DefineClass(env, "ItemQueue",
                     {
                         InstanceMethod("push", &NativeItemQueue::Push),
                         InstanceMethod("tryPop", &NativeItemQueue::TryPop),
                         InstanceMethod("size", &NativeItemQueue::Size),
                         InstanceMethod("markFinished", &NativeItemQueue::MarkFinished),
                         InstanceMethod("finished", &NativeItemQueue::Finished),
                         InstanceMethod("dispose", &NativeItemQueue::Dispose),
                     });
}

NativeItemQueue::NativeItemQueue(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<NativeItemQueue>(info) {
  Napi::Env env = info.Env();
  CallCheckedVoid(env, [&]() { impl_ = std::make_unique<foundry_local::ItemQueue>(); });
}

Napi::Value NativeItemQueue::Push(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (impl_ == nullptr) {
    Napi::TypeError::New(env, "ItemQueue: already disposed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1) {
    Napi::TypeError::New(env, "push(item: Item)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    foundry_local::Item item = JsToItem(env, info[0]);
    impl_->Push(std::move(item));
    return env.Undefined();
  });
}

Napi::Value NativeItemQueue::TryPop(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (impl_ == nullptr) {
    Napi::TypeError::New(env, "ItemQueue: already disposed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    std::optional<foundry_local::Item> popped = impl_->TryPop();
    if (!popped.has_value()) {
      return env.Null();
    }
    if (popped->GetType() == FOUNDRY_LOCAL_ITEM_QUEUE) {
      Napi::TypeError::New(env, "tryPop: nested ItemQueue is not supported on the JS surface")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    return ItemToJs(env, *popped);
  });
}

Napi::Value NativeItemQueue::Size(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (impl_ == nullptr) {
    Napi::TypeError::New(env, "ItemQueue: already disposed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    return Napi::Number::New(env, static_cast<double>(impl_->Size()));
  });
}

Napi::Value NativeItemQueue::MarkFinished(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (impl_ == nullptr) {
    Napi::TypeError::New(env, "ItemQueue: already disposed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    impl_->MarkFinished();
    return env.Undefined();
  });
}

Napi::Value NativeItemQueue::Finished(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (impl_ == nullptr) {
    Napi::TypeError::New(env, "ItemQueue: already disposed").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    return Napi::Boolean::New(env, impl_->IsFinished());
  });
}

Napi::Value NativeItemQueue::Dispose(const Napi::CallbackInfo& info) {
  // Idempotent: dropping the unique_ptr releases the native handle and
  // triggers the deleters for any pinned bytes/tensor/image/audio items
  // still in the queue. Subsequent calls hit the impl_==nullptr guards.
  impl_.reset();
  return info.Env().Undefined();
}

}  // namespace foundry_local_node
