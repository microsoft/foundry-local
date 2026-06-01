// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog.h"

#include "addon_data.h"
#include "errors.h"
#include "model.h"

#include <foundry_local/foundry_local_cpp.h>

#include <memory>
#include <string>
#include <utility>

namespace foundry_local_node {

namespace {

// Wrap a ModelList (rvalue) into a JS array of Model handles, each pinning the
// passed-in manager reference.
Napi::Value WrapModelList(Napi::Env env, foundry_local::ModelList ml,
                          Napi::ObjectReference manager) {
  auto list = std::make_shared<foundry_local::ModelList>(std::move(ml));
  auto models = list->Models();
  Napi::Array arr = Napi::Array::New(env, models.size());
  for (size_t i = 0; i < models.size(); ++i) {
    ModelCtorToken token;
    token.impl = models[i].get();
    token.keepalive = list;
    token.manager = Napi::Reference<Napi::Object>::New(manager.Value(), 1);
    arr.Set(static_cast<uint32_t>(i), Model::NewInstance(env, std::move(token)));
  }
  return arr;
}

// Wrap an owning unique_ptr<IModel> into a JS Model (or undefined when null).
Napi::Value WrapOwnedModelOrUndefined(Napi::Env env, std::unique_ptr<foundry_local::IModel> owned,
                                      Napi::ObjectReference manager) {
  if (!owned) {
    return env.Undefined();
  }
  ModelCtorToken token;
  token.impl = owned.get();
  // Wrap the unique_ptr in a shared_ptr<unique_ptr> so the keepalive can be a
  // type-erased shared_ptr<void>.
  auto holder = std::make_shared<std::unique_ptr<foundry_local::IModel>>(std::move(owned));
  token.keepalive = holder;
  token.manager = std::move(manager);
  return Model::NewInstance(env, std::move(token));
}

// Extract IModel* from a JS Model arg, or return nullptr if not a Model.
foundry_local::IModel* ExtractIModel(const Napi::Value& v) {
  if (!v.IsObject()) {
    return nullptr;
  }
  Napi::Object obj = v.As<Napi::Object>();
  auto* data = obj.Env().GetInstanceData<AddonData>();
  if (data == nullptr) {
    return nullptr;
  }
  Napi::Function ctor = data->model_ctor.Value();
  if (!obj.InstanceOf(ctor)) {
    return nullptr;
  }
  Model* m = Napi::ObjectWrap<Model>::Unwrap(obj);
  return m != nullptr ? m->native_impl() : nullptr;
}

Napi::ObjectReference CloneManager(const Napi::ObjectReference& mgr) {
  return Napi::Reference<Napi::Object>::New(mgr.Value(), 1);
}

}  // namespace

Napi::Function Catalog::Init(Napi::Env env) {
  return DefineClass(
      env, "Catalog",
      {
          InstanceMethod("getName", &Catalog::GetName),
          InstanceMethod("getModels", &Catalog::GetModels),
          InstanceMethod("getCachedModels", &Catalog::GetCachedModels),
          InstanceMethod("getLoadedModels", &Catalog::GetLoadedModels),
          InstanceMethod("getModel", &Catalog::GetModel),
          InstanceMethod("getModelVariant", &Catalog::GetModelVariant),
          InstanceMethod("getLatestVersion", &Catalog::GetLatestVersion),
      });
}

Napi::Object Catalog::NewInstance(Napi::Env env, CatalogCtorToken token) {
  auto* heap = new CatalogCtorToken(std::move(token));
  auto ext = Napi::External<CatalogCtorToken>::New(
      env, heap, [](Napi::Env /*env*/, CatalogCtorToken* t) { delete t; });
  auto* data = env.GetInstanceData<AddonData>();
  return data->catalog_ctor.New({ext});
}

Catalog::Catalog(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Catalog>(info) {
  Napi::Env env = info.Env();
  if (info.Length() != 1 || !info[0].IsExternal()) {
    Napi::TypeError::New(env, "Catalog is internal — obtain instances via Manager.getCatalog()")
        .ThrowAsJavaScriptException();
    return;
  }
  auto* token = info[0].As<Napi::External<CatalogCtorToken>>().Data();
  if (token == nullptr || token->impl == nullptr) {
    Napi::TypeError::New(env, "Catalog: invalid internal construction token").ThrowAsJavaScriptException();
    return;
  }
  impl_ = token->impl;
  manager_ = std::move(token->manager);
}

Napi::Value Catalog::GetName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    std::string_view name = impl_->GetName();
    return Napi::String::New(env, std::string(name));
  });
}

// ── ModelList getters ────────────────────────────────────────────────────────

Napi::Value Catalog::GetModels(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::ObjectReference mgr = CloneManager(manager_);
  return CallChecked<Napi::Value>(
      env, [&]() -> Napi::Value { return WrapModelList(env, impl_->GetModels(), std::move(mgr)); });
}

Napi::Value Catalog::GetCachedModels(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::ObjectReference mgr = CloneManager(manager_);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    return WrapModelList(env, impl_->GetCachedModels(), std::move(mgr));
  });
}

Napi::Value Catalog::GetLoadedModels(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::ObjectReference mgr = CloneManager(manager_);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    return WrapModelList(env, impl_->GetLoadedModels(), std::move(mgr));
  });
}

// ── Single-model lookups ─────────────────────────────────────────────────────

Napi::Value Catalog::GetModel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "getModel(alias: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string alias = info[0].As<Napi::String>();
  Napi::ObjectReference mgr = CloneManager(manager_);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    auto owned = impl_->GetModel(alias);
    return WrapOwnedModelOrUndefined(env, std::move(owned), std::move(mgr));
  });
}

Napi::Value Catalog::GetModelVariant(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "getModelVariant(modelId: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string model_id = info[0].As<Napi::String>();
  Napi::ObjectReference mgr = CloneManager(manager_);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    auto owned = impl_->GetModelVariant(model_id);
    return WrapOwnedModelOrUndefined(env, std::move(owned), std::move(mgr));
  });
}

Napi::Value Catalog::GetLatestVersion(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1) {
    Napi::TypeError::New(env, "getLatestVersion(model: Model)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  foundry_local::IModel* arg = ExtractIModel(info[0]);
  if (arg == nullptr) {
    Napi::TypeError::New(env, "getLatestVersion: argument must be a Model").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::ObjectReference mgr = CloneManager(manager_);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    auto owned = impl_->GetLatestVersion(*arg);
    return WrapOwnedModelOrUndefined(env, std::move(owned), std::move(mgr));
  });
}

}  // namespace foundry_local_node
