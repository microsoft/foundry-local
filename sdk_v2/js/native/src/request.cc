// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "request.h"

#include "addon_data.h"
#include "errors.h"
#include "item_queue.h"
#include "items.h"
#include "request_options.h"

#include <foundry_local/foundry_local_cpp.h>

#include <memory>
#include <string>
#include <utility>

namespace foundry_local_node {

Napi::Function Request::Init(Napi::Env env) {
  return DefineClass(env, "Request",
                     {
                         InstanceMethod("addItem", &Request::AddItem),
                         InstanceMethod("setOptions", &Request::SetOptions),
                         InstanceMethod("cancel", &Request::Cancel),
                         InstanceMethod("getItemCount", &Request::GetItemCount),
                         InstanceMethod("getItem", &Request::GetItem),
                     });
}

Request::Request(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Request>(info) {
  Napi::Env env = info.Env();
  CallCheckedVoid(env, [&]() { impl_ = std::make_unique<foundry_local::Request>(); });
}

Napi::Value Request::AddItem(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (impl_ == nullptr) {
    Napi::Error::New(env, "Request: not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  if (info.Length() < 1) {
    Napi::TypeError::New(env, "addItem(item: Item)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // ItemQueue is the one Item subtype that owns a native handle on the JS
  // side. The request *borrows* the queue (take_ownership=false) so the JS
  // wrapper can keep pushing into it after addItem returns.
  auto* addon_data = env.GetInstanceData<AddonData>();
  if (addon_data != nullptr && !addon_data->item_queue_ctor.IsEmpty() && info[0].IsObject() &&
      info[0].As<Napi::Object>().InstanceOf(addon_data->item_queue_ctor.Value())) {
    auto* wrapper = Napi::ObjectWrap<NativeItemQueue>::Unwrap(info[0].As<Napi::Object>());
    if (wrapper == nullptr || wrapper->impl() == nullptr) {
      Napi::TypeError::New(env, "addItem: ItemQueue is disposed").ThrowAsJavaScriptException();
      return env.Undefined();
    }
    return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
      impl_->AddItem(*wrapper->impl(), /*take_ownership=*/false);
      return env.Undefined();
    });
  }
  // JsToItem may throw a Napi::TypeError; let it propagate through CallChecked.
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    foundry_local::Item item = JsToItem(env, info[0]);
    impl_->AddItem(std::move(item));
    return env.Undefined();
  });
}

Napi::Value Request::SetOptions(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (impl_ == nullptr) {
    Napi::Error::New(env, "Request: not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
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

Napi::Value Request::Cancel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (impl_ == nullptr) {
    return env.Undefined();
  }
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    impl_->Cancel();
    return env.Undefined();
  });
}

Napi::Value Request::GetItemCount(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (impl_ == nullptr) {
    return Napi::Number::New(env, 0);
  }
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    return Napi::Number::New(env, static_cast<double>(impl_->GetItemCount()));
  });
}

Napi::Value Request::GetItem(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (impl_ == nullptr) {
    return env.Undefined();
  }
  if (info.Length() < 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "getItem(index: number)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  uint32_t idx = info[0].As<Napi::Number>().Uint32Value();
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    foundry_local::Item item = impl_->GetItem(idx);
    return ItemToJs(env, item);
  });
}

}  // namespace foundry_local_node
