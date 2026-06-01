// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Napi::ObjectWrap<Catalog> over the C++ wrapper's foundry_local::ICatalog.
//
// The Catalog wrapper does NOT own the underlying flCatalog* — it is owned by
// the Manager (Manager::GetCatalog() returns an internal reference). This JS
// wrapper holds a raw ICatalog* plus a Napi::ObjectReference pinning the
// parent Manager so the catalog cannot outlive its owner.
//
// Constructed only by Manager.getCatalog() / .getCatalogSync(). User code that
// calls `new Catalog(...)` gets a TypeError.
#pragma once

#include <napi.h>

#include <foundry_local/foundry_local_cpp.h>

#include <utility>

namespace foundry_local_node {

struct CatalogCtorToken {
  foundry_local::ICatalog* impl = nullptr;
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
  Napi::Value GetModel(const Napi::CallbackInfo& info);
  Napi::Value GetModelVariant(const Napi::CallbackInfo& info);
  Napi::Value GetLatestVersion(const Napi::CallbackInfo& info);

  foundry_local::ICatalog* impl_ = nullptr;
  Napi::ObjectReference manager_;
};

}  // namespace foundry_local_node
