// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Used by asynchronous catalog queries and model load/unload operations.
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
  using CompletionCheck = std::function<void()>;

  static Napi::Promise Run(Napi::Env env, Job job, Resolver resolver,
                           Napi::ObjectReference owner = Napi::ObjectReference(),
                           CompletionCheck completion_check = CompletionCheck()) {
    auto* w = new PromiseWorker(env, std::move(job), std::move(resolver), std::move(owner),
                                std::move(completion_check));
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
    if (completion_check_) {
      try {
        completion_check_();
      } catch (const foundry_local::Error& e) {
        deferred_.Reject(MakeFoundryLocalError(env, static_cast<int>(e.Code()), e.what()).Value());
        return;
      } catch (const std::exception& e) {
        deferred_.Reject(Napi::Error::New(env, e.what()).Value());
        return;
      }
    }
    Napi::Value value = resolver_(env, *result_);
    deferred_.Resolve(value);
  }

  void OnError(const Napi::Error& /*unused*/) override {
    Napi::Env env = Env();
    Napi::HandleScope scope(env);
    if (tagged_) {
      deferred_.Reject(MakeFoundryLocalError(env, err_code_, err_msg_).Value());
    } else {
      deferred_.Reject(Napi::Error::New(env, err_msg_).Value());
    }
  }

 private:
  PromiseWorker(Napi::Env env, Job job, Resolver resolver, Napi::ObjectReference owner,
                CompletionCheck completion_check)
      : Napi::AsyncWorker(env),
        deferred_(Napi::Promise::Deferred::New(env)),
        job_(std::move(job)),
        resolver_(std::move(resolver)),
        owner_(std::move(owner)),
        completion_check_(std::move(completion_check)) {}

  Napi::Promise::Deferred deferred_;
  Job job_;
  Resolver resolver_;
  Napi::ObjectReference owner_;  // pins parent ObjectWrap alive across the worker
  CompletionCheck completion_check_;
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
  using Completion = std::function<void()>;

  static Napi::Promise Run(Napi::Env env, Job job,
                           Napi::ObjectReference owner = Napi::ObjectReference(),
                           Completion completion = Completion()) {
    auto* w = new PromiseWorkerVoid(env, std::move(job), std::move(owner), std::move(completion));
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
    if (completion_) completion_();
    deferred_.Resolve(env.Undefined());
  }

  void OnError(const Napi::Error& /*unused*/) override {
    Napi::Env env = Env();
    Napi::HandleScope scope(env);
    if (completion_) completion_();
    if (tagged_) {
      deferred_.Reject(MakeFoundryLocalError(env, err_code_, err_msg_).Value());
    } else {
      deferred_.Reject(Napi::Error::New(env, err_msg_).Value());
    }
  }

 private:
  PromiseWorkerVoid(Napi::Env env, Job job, Napi::ObjectReference owner, Completion completion)
      : Napi::AsyncWorker(env),
        deferred_(Napi::Promise::Deferred::New(env)),
        job_(std::move(job)),
        owner_(std::move(owner)),
        completion_(std::move(completion)) {}

  Napi::Promise::Deferred deferred_;
  Job job_;
  Napi::ObjectReference owner_;
  Completion completion_;
  std::string err_msg_;
  int err_code_ = 0;
  bool tagged_ = false;
};

}  // namespace foundry_local_node
