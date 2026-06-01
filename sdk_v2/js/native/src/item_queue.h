// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Napi::ObjectWrap<NativeItemQueue> over foundry_local::ItemQueue.
//
// Unlike the plain-object `Item` shapes exposed to JS, an ItemQueue holds
// stateful push/pop/finished state and must be addressable by both producer
// and consumer threads — so it gets a real native handle exposed to JS via
// ObjectWrap. The JS-side `ItemQueue` class wraps this; `Request.addItem`
// branches on InstanceOf(NativeItemQueue) and borrows the handle without
// transferring ownership (the JS side keeps the queue alive).
#pragma once

#include <napi.h>

#include <foundry_local/foundry_local_cpp.h>

#include <memory>

namespace foundry_local_node {

class NativeItemQueue : public Napi::ObjectWrap<NativeItemQueue> {
 public:
  static Napi::Function Init(Napi::Env env);
  explicit NativeItemQueue(const Napi::CallbackInfo& info);

  // Accessor for request.cc to borrow the underlying queue without taking
  // ownership. Returns nullptr after `dispose()`.
  foundry_local::ItemQueue* impl() noexcept { return impl_.get(); }

 private:
  Napi::Value Push(const Napi::CallbackInfo& info);
  Napi::Value TryPop(const Napi::CallbackInfo& info);
  Napi::Value Size(const Napi::CallbackInfo& info);
  Napi::Value MarkFinished(const Napi::CallbackInfo& info);
  Napi::Value Finished(const Napi::CallbackInfo& info);
  Napi::Value Dispose(const Napi::CallbackInfo& info);

  std::unique_ptr<foundry_local::ItemQueue> impl_;
};

}  // namespace foundry_local_node
