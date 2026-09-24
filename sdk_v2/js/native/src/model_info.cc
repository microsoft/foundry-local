// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "model_info.h"

#include "errors.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace foundry_local_node {

Napi::Function NativeModelInfo::Init(Napi::Env env) {
  return DefineClass(env, "ModelInfo",
                     {
                         InstanceMethod("setStringProperty", &NativeModelInfo::SetStringProperty),
                         InstanceMethod("setIntProperty", &NativeModelInfo::SetIntProperty),
                         InstanceMethod("dispose", &NativeModelInfo::Dispose),
                         InstanceMethod("isDisposed", &NativeModelInfo::IsDisposed),
                         InstanceMethod("failNextSnapshotForTest", &NativeModelInfo::FailNextSnapshotForTest),
                         InstanceMethod("setIntPropertyForTest", &NativeModelInfo::SetIntPropertyForTest),
                     });
}

NativeModelInfo::NativeModelInfo(const Napi::CallbackInfo& info) : Napi::ObjectWrap<NativeModelInfo>(info) {
  Napi::Env env = info.Env();
  if (info.Length() != 0) {
    Napi::TypeError::New(env, "ModelInfo constructor does not accept arguments").ThrowAsJavaScriptException();
    return;
  }
  CallCheckedVoid(env, [&]() { impl_ = std::make_shared<foundry_local::ModelInfo>(); });
}

std::shared_ptr<foundry_local::ModelInfo> NativeModelInfo::Snapshot() const {
  if (impl_ == nullptr) return nullptr;
  if (fail_next_snapshot_for_test_) {
    fail_next_snapshot_for_test_ = false;
    throw foundry_local::Error("Injected ModelInfo snapshot failure", FOUNDRY_LOCAL_ERROR_INTERNAL);
  }
  auto snapshot = std::make_shared<foundry_local::ModelInfo>();
  for (const auto& [key, value] : string_properties_) {
    snapshot->SetStringProperty(key.c_str(), value.c_str());
  }
  for (const auto& [key, value] : int_properties_) {
    snapshot->SetIntProperty(key.c_str(), value);
  }
  return snapshot;
}

bool NativeModelInfo::ThrowIfDisposed(Napi::Env env) const {
  if (impl_ == nullptr) {
    Napi::TypeError::New(env, "ModelInfo has been disposed").ThrowAsJavaScriptException();
    return true;
  }
  return false;
}

Napi::Value NativeModelInfo::SetStringProperty(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  if (info.Length() != 2 || !info[0].IsString() || !info[1].IsString()) {
    Napi::TypeError::New(env, "setStringProperty(key: string, value: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string key = info[0].As<Napi::String>();
  std::string value = info[1].As<Napi::String>();
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    impl_->SetStringProperty(key.c_str(), value.c_str());
    string_properties_[key] = value;
    return info.This();
  });
}

Napi::Value NativeModelInfo::SetIntProperty(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  if (info.Length() != 2 || !info[0].IsString() || !info[1].IsNumber()) {
    Napi::TypeError::New(env, "setIntProperty(key: string, value: safe integer)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  const double raw = info[1].As<Napi::Number>().DoubleValue();
  if (!std::isfinite(raw) || std::trunc(raw) != raw || raw < static_cast<double>(std::numeric_limits<int64_t>::min()) ||
      raw > static_cast<double>(std::numeric_limits<int64_t>::max()) || std::abs(raw) > 9007199254740991.0) {
    Napi::TypeError::New(env, "setIntProperty: value must be a safe integer").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::string key = info[0].As<Napi::String>();
  const auto value = static_cast<int64_t>(raw);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    impl_->SetIntProperty(key.c_str(), value);
    int_properties_[key] = value;
    return info.This();
  });
}

Napi::Value NativeModelInfo::Dispose(const Napi::CallbackInfo& info) {
  impl_.reset();
  string_properties_.clear();
  int_properties_.clear();
  return info.Env().Undefined();
}

Napi::Value NativeModelInfo::IsDisposed(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), impl_ == nullptr);
}

Napi::Value NativeModelInfo::FailNextSnapshotForTest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) {
    return env.Undefined();
  }
  fail_next_snapshot_for_test_ = true;
  return env.Undefined();
}

Napi::Value NativeModelInfo::SetIntPropertyForTest(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) return env.Undefined();
  if (info.Length() != 2 || !info[0].IsString() || !info[1].IsBigInt()) {
    Napi::TypeError::New(env, "setIntPropertyForTest(key: string, value: bigint)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  bool lossless = false;
  const int64_t value = info[1].As<Napi::BigInt>().Int64Value(&lossless);
  if (!lossless) {
    Napi::RangeError::New(env, "setIntPropertyForTest: value must fit in int64").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::string key = info[0].As<Napi::String>();
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    impl_->SetIntProperty(key.c_str(), value);
    int_properties_[key] = value;
    return info.This();
  });
}

}  // namespace foundry_local_node