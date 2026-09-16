// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Napi::ObjectWrap<Catalog> over the C++ wrapper's foundry_local::ICatalog.
//
// The Catalog wrapper stores its catalog type and weak native Manager ownership. Each operation acquires a strong
// Manager lease before resolving its manager-owned ICatalog reference, so explicit disposal rejects new calls while
// already queued workers can finish safely.
//
// Constructed only by Manager.getCatalog() / .getCatalogSync(). User code that
// calls `new Catalog(...)` gets a TypeError.
#pragma once

#include <napi.h>

#include <foundry_local/foundry_local_c.h>
#include <foundry_local/foundry_local_cpp.h>

#include <atomic>
#include <memory>
#include <utility>

namespace foundry_local_node {

struct CatalogCtorToken {
  flCatalogType catalog_type = FOUNDRY_LOCAL_CATALOG_PUBLIC;
  std::weak_ptr<foundry_local::Manager> manager_lifetime;
  std::shared_ptr<std::atomic_bool> disposed;
  Napi::ObjectReference manager;  // pins the owning Manager
};

class Catalog : public Napi::ObjectWrap<Catalog> {
 public:
  static Napi::Function Init(Napi::Env env);
  static Napi::Object NewInstance(Napi::Env env, CatalogCtorToken token);

  explicit Catalog(const Napi::CallbackInfo& info);

 private:
  Napi::Value GetName(const Napi::CallbackInfo& info);
  Napi::Value GetModels(const Napi::CallbackInfo& info);
  Napi::Value GetCachedModels(const Napi::CallbackInfo& info);
  Napi::Value GetLoadedModels(const Napi::CallbackInfo& info);
  Napi::Value GetModelVersions(const Napi::CallbackInfo& info);
  Napi::Value GetModel(const Napi::CallbackInfo& info);
  Napi::Value GetModelVariant(const Napi::CallbackInfo& info);
  Napi::Value GetLatestVersion(const Napi::CallbackInfo& info);
  Napi::Value RegisterModel(const Napi::CallbackInfo& info);
  Napi::Value RegisterModelSync(const Napi::CallbackInfo& info);
  Napi::Value UnregisterModel(const Napi::CallbackInfo& info);
  Napi::Value UnregisterModelSync(const Napi::CallbackInfo& info);

  std::shared_ptr<foundry_local::Manager> LockManager(Napi::Env env) const;

  flCatalogType catalog_type_ = FOUNDRY_LOCAL_CATALOG_PUBLIC;
  std::weak_ptr<foundry_local::Manager> manager_lifetime_;
  std::shared_ptr<std::atomic_bool> disposed_;
  Napi::ObjectReference manager_;
};

}  // namespace foundry_local_node
