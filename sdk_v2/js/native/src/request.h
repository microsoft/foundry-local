// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Napi::ObjectWrap<Request> over foundry_local::Request. Mirrors the C# Request:
// a stateful builder that owns a flRequest handle. JS callers construct one,
// append input items, optionally set options, and pass it to
// Session.processRequest() or Session.processStreamingRequest().
//
// Items are owned by the Request once added (take_ownership=true), so the C++
// wrapper's destructor releases everything when the JS Request is GC'd.
#pragma once

#include <napi.h>

#include <foundry_local/foundry_local_cpp.h>

#include <memory>

namespace foundry_local_node {

class Request : public Napi::ObjectWrap<Request> {
 public:
  static Napi::Function Init(Napi::Env env);
  explicit Request(const Napi::CallbackInfo& info);

  // Internal accessor used by Session.processRequest() to forward the native handle.
  // Returns nullptr if the Request has been disposed or never initialized.
  foundry_local::Request* native() noexcept { return impl_.get(); }

 private:
  Napi::Value AddItem(const Napi::CallbackInfo& info);
  Napi::Value SetOptions(const Napi::CallbackInfo& info);
  Napi::Value Cancel(const Napi::CallbackInfo& info);
  Napi::Value GetItemCount(const Napi::CallbackInfo& info);
  Napi::Value GetItem(const Napi::CallbackInfo& info);

  std::unique_ptr<foundry_local::Request> impl_;
};

}  // namespace foundry_local_node
