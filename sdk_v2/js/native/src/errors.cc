// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "errors.h"

#include <foundry_local/foundry_local_cpp.h>

#include <exception>
#include <string>

namespace foundry_local_node {

Napi::Error MakeFoundryLocalError(Napi::Env env, int code, const std::string& message) {
  Napi::Error err = Napi::Error::New(env, message);
  Napi::Object value = err.Value();
  value.Set("name", Napi::String::New(env, "FoundryLocalError"));
  value.Set("code", Napi::Number::New(env, code));
  return err;
}

void ThrowFoundryLocalError(Napi::Env env, int code, const std::string& message) {
  MakeFoundryLocalError(env, code, message).ThrowAsJavaScriptException();
}

template <typename T>
T CallChecked(Napi::Env env, const std::function<T()>& fn) {
  try {
    return fn();
  } catch (const foundry_local::Error& e) {
    ThrowFoundryLocalError(env, static_cast<int>(e.Code()), e.what());
  } catch (const Napi::Error& e) {
    e.ThrowAsJavaScriptException();
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
  } catch (...) {
    Napi::Error::New(env, "Unknown native exception").ThrowAsJavaScriptException();
  }
  return T{};
}

void CallCheckedVoid(Napi::Env env, const std::function<void()>& fn) {
  try {
    fn();
  } catch (const foundry_local::Error& e) {
    ThrowFoundryLocalError(env, static_cast<int>(e.Code()), e.what());
  } catch (const Napi::Error& e) {
    e.ThrowAsJavaScriptException();
  } catch (const std::exception& e) {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
  } catch (...) {
    Napi::Error::New(env, "Unknown native exception").ThrowAsJavaScriptException();
  }
}

// Explicit instantiations for the result types used across the addon.
template Napi::Value CallChecked<Napi::Value>(Napi::Env, const std::function<Napi::Value()>&);
template Napi::Object CallChecked<Napi::Object>(Napi::Env, const std::function<Napi::Object()>&);
template Napi::String CallChecked<Napi::String>(Napi::Env, const std::function<Napi::String()>&);
template Napi::Array CallChecked<Napi::Array>(Napi::Env, const std::function<Napi::Array()>&);
template Napi::Boolean CallChecked<Napi::Boolean>(Napi::Env, const std::function<Napi::Boolean()>&);

}  // namespace foundry_local_node
