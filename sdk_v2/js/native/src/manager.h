// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Napi::ObjectWrap<Manager> over std::unique_ptr<foundry_local::Manager>.
//
// Surface:
//  - ctor accepts { appName, modelCacheDir?, serviceEndpoint? }
//  - getWebServiceEndpoints (sync; underlying call is an in-memory vector copy)
//  - getCatalog (sync; returns a typed Catalog handle)
//  - dispose() — idempotent; subsequent operation calls throw a
//    FoundryLocalError tagged with FOUNDRY_LOCAL_ERROR_INVALID_USAGE.
#pragma once

#include <napi.h>

#include <foundry_local/foundry_local_cpp.h>

#include <memory>

namespace foundry_local_node {

class Manager : public Napi::ObjectWrap<Manager> {
 public:
  static Napi::Function Init(Napi::Env env);
  explicit Manager(const Napi::CallbackInfo& info);

 private:
  // Sync entry: calls Manager::GetWebServiceEndpoints inline on the JS thread.
  // The underlying C ABI call is an in-memory vector copy — no I/O, no async
  // payoff. Real-work async (download/load/inference) lives on Model/Session.
  Napi::Value GetWebServiceEndpoints(const Napi::CallbackInfo& info);

  // Sync entry: returns a typed Catalog handle pinning this Manager.
  Napi::Value GetCatalog(const Napi::CallbackInfo& info);

  // Web service control. All sync — underlying C ABI calls are quick.
  Napi::Value StartWebService(const Napi::CallbackInfo& info);
  Napi::Value StopWebService(const Napi::CallbackInfo& info);

  // EP discovery & registration.
  Napi::Value DiscoverEps(const Napi::CallbackInfo& info);
  Napi::Value DownloadAndRegisterEps(const Napi::CallbackInfo& info);
  Napi::Value IsEpDownloadInProgress(const Napi::CallbackInfo& info);

  // Graceful shutdown.
  Napi::Value Shutdown(const Napi::CallbackInfo& info);
  Napi::Value IsShutdownRequested(const Napi::CallbackInfo& info);

  // Idempotent native-side teardown. Releases the underlying foundry_local::Manager.
  // After dispose() every other instance method throws a tagged FoundryLocalError.
  Napi::Value Dispose(const Napi::CallbackInfo& info);
  Napi::Value IsDisposed(const Napi::CallbackInfo& info);

  // If the Manager has been disposed, throws a tagged FoundryLocalError pending
  // on env and returns true. Callers should return env.Undefined() when true.
  bool ThrowIfDisposed(Napi::Env env);

  std::unique_ptr<foundry_local::Manager> impl_;
};

}  // namespace foundry_local_node
