// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Napi::ObjectWrap<Model> over the C++ wrapper's IModel.
//
// A JS Model is NOT directly constructible from JS — its constructor only
// accepts an Napi::External<ModelCtorToken> token that the addon creates
// internally (via Model::NewInstance from Catalog methods or from Model
// methods that produce new Model instances such as GetVariants). User code
// that calls `new Model(...)` gets a TypeError.
//
// Lifetime: a Model handle may have been obtained either as an owning
// std::unique_ptr<IModel> (from Catalog::GetModel / GetModelVariant /
// GetLatestVersion) or as a non-owning view into a foundry_local::ModelList
// (from Catalog::GetModels / GetCachedModels / GetLoadedModels /
// Model::GetVariants). Both cases are represented by a raw IModel* accessor
// + a std::shared_ptr<void> keepalive that holds whichever owning object is
// responsible for the underlying flModel pointer.
//
// Exposed methods: getInfo, isCached, isLoaded, getPath, getVariants,
// selectVariant, download (with optional progress callback), load, unload,
// removeFromCache. GetInputOutputInfo from the C++ wrapper is intentionally
// not surfaced — add only when a JS consumer scenario requires it.
#pragma once

#include <napi.h>

#include <foundry_local/foundry_local_cpp.h>

#include <atomic>
#include <memory>
#include <utility>

namespace foundry_local_node {

struct ModelCtorToken {
  // The IModel accessor. Never null when the token is constructed.
  foundry_local::IModel* impl = nullptr;
  // Keeps the owning object (std::unique_ptr<IModel> wrapped in a shared_ptr,
  // or a std::shared_ptr<foundry_local::ModelList>) alive for the JS Model's
  // lifetime.
  std::shared_ptr<void> keepalive;
  // Models hold weak native ownership and reject new operations after explicit disposal.
  std::weak_ptr<foundry_local::Manager> manager_lifetime;
  std::shared_ptr<std::atomic_bool> disposed;
  // Pins the parent Manager JavaScript object for wrapper reachability.
  Napi::ObjectReference manager;
};

class Model : public Napi::ObjectWrap<Model> {
 public:
  static Napi::Function Init(Napi::Env env);

  // Build a JS Model from the addon side. Wraps the token in an External and
  // calls the registered Model constructor (stored on AddonData).
  static Napi::Object NewInstance(Napi::Env env, ModelCtorToken token);

  explicit Model(const Napi::CallbackInfo& info);

  // Internal accessor used when one Model needs to be passed back to a
  // Catalog method (e.g. GetLatestVersion). Returns nullptr if the wrapper
  // is in an invalid state.
  foundry_local::IModel* native_impl(Napi::Env env) const;

  // Internal accessors used by session constructors to retain native ownership and observe explicit disposal.
  std::shared_ptr<foundry_local::Manager> LockManager(Napi::Env env) const;
  const std::shared_ptr<std::atomic_bool>& disposed_state() const noexcept { return disposed_; }
  const Napi::ObjectReference& manager() const noexcept { return manager_; }

 private:
  Napi::Value GetInfo(const Napi::CallbackInfo& info);
  Napi::Value GetStringProperty(const Napi::CallbackInfo& info);
  Napi::Value GetIntProperty(const Napi::CallbackInfo& info);
  Napi::Value IsCached(const Napi::CallbackInfo& info);
  Napi::Value IsLoaded(const Napi::CallbackInfo& info);
  Napi::Value GetPath(const Napi::CallbackInfo& info);
  Napi::Value GetVariants(const Napi::CallbackInfo& info);
  Napi::Value SelectVariant(const Napi::CallbackInfo& info);
  // Async model lifecycle.
  Napi::Value Load(const Napi::CallbackInfo& info);
  Napi::Value Unload(const Napi::CallbackInfo& info);
  Napi::Value Download(const Napi::CallbackInfo& info);
  Napi::Value RemoveFromCache(const Napi::CallbackInfo& info);

  foundry_local::IModel* impl_ = nullptr;
  std::shared_ptr<void> keepalive_;
  std::weak_ptr<foundry_local::Manager> manager_lifetime_;
  std::shared_ptr<std::atomic_bool> disposed_;
  Napi::ObjectReference manager_;
};

}  // namespace foundry_local_node
