// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Mutable, caller-owned ModelInfo used when registering local models.
#pragma once

#include <napi.h>

#include <foundry_local/foundry_local_cpp.h>

#include <memory>
#include <map>
#include <string>

namespace foundry_local_node {

class NativeModelInfo : public Napi::ObjectWrap<NativeModelInfo> {
 public:
  static Napi::Function Init(Napi::Env env);
  explicit NativeModelInfo(const Napi::CallbackInfo& info);

  std::shared_ptr<foundry_local::ModelInfo> shared_impl() const noexcept { return impl_; }
  std::shared_ptr<foundry_local::ModelInfo> Snapshot() const;

 private:
  Napi::Value SetStringProperty(const Napi::CallbackInfo& info);
  Napi::Value SetIntProperty(const Napi::CallbackInfo& info);
  Napi::Value Dispose(const Napi::CallbackInfo& info);
  Napi::Value IsDisposed(const Napi::CallbackInfo& info);

  bool ThrowIfDisposed(Napi::Env env) const;

  // Async registration snapshots this state before queuing work.
  std::shared_ptr<foundry_local::ModelInfo> impl_;
  std::map<std::string, std::string> string_properties_;
  std::map<std::string, int64_t> int_properties_;
};

}  // namespace foundry_local_node