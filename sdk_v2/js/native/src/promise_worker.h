// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Used by model download/load/unload and session inference.
//
// PromiseWorker<T> — generic Napi::AsyncWorker that runs a std::function<T()>
// on a libuv worker thread and resolves / rejects a JS Promise with the
// result. foundry_local::Error is preserved across the worker boundary so
// rejections carry the same `name === "FoundryLocalError"` / `code` tags the
// sync entries produce via errors.cc.
//
// Notes:
//  - For void-returning jobs use the PromiseWorkerVoid specialization below.
//  - Resolution converter runs on the JS thread (HandleScope already active)
//    and receives the result by mutable reference so move-only T (e.g.
//    std::vector<std::unique_ptr<...>>) is cheap to consume.
//  - `owner` is an optional ObjectReference used to pin a parent ObjectWrap
//    (e.g. Manager) alive while the worker is in flight; pass an empty
//    Napi::ObjectReference if no parent pinning is required.
#pragma once

#include "errors.h"

#include <napi.h>

#include <foundry_local/foundry_local_cpp.h>

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace foundry_local_node {

template <typename T>
class PromiseWorker : public Napi::AsyncWorker {
 public:
  using Job = std::function<T()>;
  using Resolver = std::function<Napi::Value(Napi::Env, T&)>;

  static Napi::Promise Run(Napi::Env env, Job job, Resolver resolver,
                           Napi::ObjectReference owner = Napi::ObjectReference()) {
    auto* w = new PromiseWorker(env, std::move(job), std::move(resolver), std::move(owner));
    Napi::Promise p = w->deferred_.Promise();
    w->Queue();
    return p;
  }

  void Execute() override {
    try {
      result_ = std::make_unique<T>(job_());
    } catch (const foundry_local::Error& e) {
      err_code_ = static_cast<int>(e.Code());
      err_msg_ = e.what();
      tagged_ = true;
      SetError(err_msg_);
    } catch (const std::exception& e) {
      err_msg_ = e.what();
      SetError(err_msg_);
    } catch (...) {
      err_msg_ = "Unknown native exception";
      SetError(err_msg_);
    }
  }

  void OnOK() override {
    Napi::Env env = Env();
    Napi::HandleScope scope(env);
    Napi::Value value = resolver_(env, *result_);
    deferred_.Resolve(value);
  }

  void OnError(const Napi::Error& /*unused*/) override {
    Napi::Env env = Env();
    Napi::HandleScope scope(env);
    if (tagged_) {
      Napi::Error err = Napi::Error::New(env, err_msg_);
      Napi::Object value = err.Value();
      value.Set("name", Napi::String::New(env, "FoundryLocalError"));
      value.Set("code", Napi::Number::New(env, err_code_));
      deferred_.Reject(value);
    } else {
      deferred_.Reject(Napi::Error::New(env, err_msg_).Value());
    }
  }

 private:
  PromiseWorker(Napi::Env env, Job job, Resolver resolver, Napi::ObjectReference owner)
      : Napi::AsyncWorker(env),
        deferred_(Napi::Promise::Deferred::New(env)),
        job_(std::move(job)),
        resolver_(std::move(resolver)),
        owner_(std::move(owner)) {}

  Napi::Promise::Deferred deferred_;
  Job job_;
  Resolver resolver_;
  Napi::ObjectReference owner_;  // pins parent ObjectWrap alive across the worker
  std::unique_ptr<T> result_;    // holds the move-only result between Execute and OnOK
  std::string err_msg_;
  int err_code_ = 0;
  bool tagged_ = false;  // true iff err_msg_ originated from a foundry_local::Error
};

/// Void specialization: runs a std::function<void()> on a worker thread and
/// resolves the Promise with `undefined` on success. Error tagging matches
/// PromiseWorker<T>.
class PromiseWorkerVoid : public Napi::AsyncWorker {
 public:
  using Job = std::function<void()>;

  static Napi::Promise Run(Napi::Env env, Job job,
                           Napi::ObjectReference owner = Napi::ObjectReference()) {
    auto* w = new PromiseWorkerVoid(env, std::move(job), std::move(owner));
    Napi::Promise p = w->deferred_.Promise();
    w->Queue();
    return p;
  }

  void Execute() override {
    try {
      job_();
    } catch (const foundry_local::Error& e) {
      err_code_ = static_cast<int>(e.Code());
      err_msg_ = e.what();
      tagged_ = true;
      SetError(err_msg_);
    } catch (const std::exception& e) {
      err_msg_ = e.what();
      SetError(err_msg_);
    } catch (...) {
      err_msg_ = "Unknown native exception";
      SetError(err_msg_);
    }
  }

  void OnOK() override {
    Napi::Env env = Env();
    Napi::HandleScope scope(env);
    deferred_.Resolve(env.Undefined());
  }

  void OnError(const Napi::Error& /*unused*/) override {
    Napi::Env env = Env();
    Napi::HandleScope scope(env);
    if (tagged_) {
      Napi::Error err = Napi::Error::New(env, err_msg_);
      Napi::Object value = err.Value();
      value.Set("name", Napi::String::New(env, "FoundryLocalError"));
      value.Set("code", Napi::Number::New(env, err_code_));
      deferred_.Reject(value);
    } else {
      deferred_.Reject(Napi::Error::New(env, err_msg_).Value());
    }
  }

 private:
  PromiseWorkerVoid(Napi::Env env, Job job, Napi::ObjectReference owner)
      : Napi::AsyncWorker(env),
        deferred_(Napi::Promise::Deferred::New(env)),
        job_(std::move(job)),
        owner_(std::move(owner)) {}

  Napi::Promise::Deferred deferred_;
  Job job_;
  Napi::ObjectReference owner_;
  std::string err_msg_;
  int err_code_ = 0;
  bool tagged_ = false;
};

}  // namespace foundry_local_node
