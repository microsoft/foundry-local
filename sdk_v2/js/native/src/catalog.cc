// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "catalog.h"

#include "addon_data.h"
#include "errors.h"
#include "model.h"
#include "model_info.h"
#include "promise_worker.h"
#include "worker_start_gate.h"

#include <foundry_local/foundry_local_c.h>
#include <foundry_local/foundry_local_cpp.h>

#include <memory>
#include <string>
#include <utility>

namespace foundry_local_node {

namespace {

// Wrap a ModelList (rvalue) into a JS array of Model handles, each pinning the
// passed-in manager reference.
Napi::Value WrapModelList(Napi::Env env, foundry_local::ModelList ml, Napi::ObjectReference manager,
                          std::weak_ptr<foundry_local::Manager> manager_lifetime,
                          std::shared_ptr<std::atomic_bool> disposed) {
  auto list = std::make_shared<foundry_local::ModelList>(std::move(ml));
  const auto& models = *list;
  Napi::Array arr = Napi::Array::New(env, models.size());
  for (size_t i = 0; i < models.size(); ++i) {
    ModelCtorToken token;
    token.impl = models[i].get();
    token.keepalive = list;
    token.manager_lifetime = manager_lifetime;
    token.disposed = disposed;
    token.manager = Napi::Reference<Napi::Object>::New(manager.Value(), 1);
    arr.Set(static_cast<uint32_t>(i), Model::NewInstance(env, std::move(token)));
  }
  return arr;
}

// Wrap an owning unique_ptr<IModel> into a JS Model (or undefined when null).
Napi::Value WrapOwnedModelOrUndefined(Napi::Env env, std::unique_ptr<foundry_local::IModel> owned,
                                      Napi::ObjectReference manager,
                                      std::weak_ptr<foundry_local::Manager> manager_lifetime,
                                      std::shared_ptr<std::atomic_bool> disposed) {
  if (!owned) {
    return env.Undefined();
  }
  ModelCtorToken token;
  token.impl = owned.get();
  // Wrap the unique_ptr in a shared_ptr<unique_ptr> so the keepalive can be a
  // type-erased shared_ptr<void>.
  auto holder = std::make_shared<std::unique_ptr<foundry_local::IModel>>(std::move(owned));
  token.keepalive = holder;
  token.manager_lifetime = std::move(manager_lifetime);
  token.disposed = std::move(disposed);
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
  return m != nullptr ? m->native_impl(v.Env()) : nullptr;
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
          InstanceMethod("getModelVersions", &Catalog::GetModelVersions),
          InstanceMethod("getModel", &Catalog::GetModel),
          InstanceMethod("getModelVariant", &Catalog::GetModelVariant),
          InstanceMethod("getLatestVersion", &Catalog::GetLatestVersion),
          InstanceMethod("registerModel", &Catalog::RegisterModel),
          InstanceMethod("registerModelSync", &Catalog::RegisterModelSync),
          InstanceMethod("unregisterModel", &Catalog::UnregisterModel),
          InstanceMethod("unregisterModelSync", &Catalog::UnregisterModelSync),
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
  if (token == nullptr || token->manager_lifetime.expired() || !token->disposed) {
    Napi::TypeError::New(env, "Catalog: invalid internal construction token").ThrowAsJavaScriptException();
    return;
  }
  catalog_type_ = token->catalog_type;
  manager_lifetime_ = std::move(token->manager_lifetime);
  disposed_ = std::move(token->disposed);
  manager_ = std::move(token->manager);
}

std::shared_ptr<foundry_local::Manager> Catalog::LockManager(Napi::Env env) const {
  if (disposed_->load()) {
    ThrowFoundryLocalError(env, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Manager has been disposed");
    return nullptr;
  }
  auto manager = manager_lifetime_.lock();
  if (!manager) {
    ThrowFoundryLocalError(env, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Manager has been disposed");
  }
  return manager;
}

Napi::Value Catalog::GetName(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  auto manager = LockManager(env);
  if (!manager) {
    return env.Undefined();
  }
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    std::string_view name = manager->GetCatalog(catalog_type_).GetName();
    return Napi::String::New(env, std::string(name));
  });
}

// ── ModelList getters ────────────────────────────────────────────────────────

Napi::Value Catalog::GetModels(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  auto manager = LockManager(env);
  if (!manager) {
    return env.Undefined();
  }
  Napi::ObjectReference mgr = CloneManager(manager_);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    return WrapModelList(env, manager->GetCatalog(catalog_type_).GetModels(), std::move(mgr), manager, disposed_);
  });
}

Napi::Value Catalog::GetCachedModels(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  auto manager = LockManager(env);
  if (!manager) {
    return env.Undefined();
  }
  Napi::ObjectReference mgr = CloneManager(manager_);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    return WrapModelList(env, manager->GetCatalog(catalog_type_).GetCachedModels(), std::move(mgr), manager,
               disposed_);
  });
}

Napi::Value Catalog::GetLoadedModels(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  auto manager = LockManager(env);
  if (!manager) {
    return env.Undefined();
  }
  Napi::ObjectReference mgr = CloneManager(manager_);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    return WrapModelList(env, manager->GetCatalog(catalog_type_).GetLoadedModels(), std::move(mgr), manager,
               disposed_);
  });
}

Napi::Value Catalog::GetModelVersions(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "getModelVersions(modelAlias: string, modelName?: string | null, maxVersions?: number)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::string model_alias = info[0].As<Napi::String>();
  std::string variant_name;
  if (info.Length() >= 2 && !info[1].IsNull() && !info[1].IsUndefined()) {
    if (!info[1].IsString()) {
      Napi::TypeError::New(env, "getModelVersions: modelName must be a string, null, or undefined")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    variant_name = info[1].As<Napi::String>();
  }

  int max_versions = 50;
  if (info.Length() >= 3 && !info[2].IsUndefined()) {
    if (!info[2].IsNumber()) {
      Napi::TypeError::New(env, "getModelVersions: maxVersions must be a number")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    max_versions = static_cast<int>(info[2].As<Napi::Number>().Int32Value());
  }

  auto manager = LockManager(env);
  if (!manager) {
    return env.Undefined();
  }
  auto manager_pin = std::make_shared<Napi::ObjectReference>(CloneManager(manager_));
  return PromiseWorker<foundry_local::ModelList>::Run(
      env,
      [manager, catalog_type = catalog_type_, model_alias, variant_name, max_versions]() -> foundry_local::ModelList {
        return manager->GetCatalog(catalog_type).GetModelVersions(model_alias, variant_name, max_versions);
      },
      [manager_pin, manager_lifetime = std::weak_ptr<foundry_local::Manager>(manager), disposed = disposed_](
          Napi::Env env, foundry_local::ModelList& ml) -> Napi::Value {
        return WrapModelList(env, std::move(ml), CloneManager(*manager_pin), manager_lifetime, disposed);
      },
      CloneManager(manager_));
}

// ── Single-model lookups ─────────────────────────────────────────────────────

Napi::Value Catalog::GetModel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "getModel(alias: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string alias = info[0].As<Napi::String>();
  auto manager = LockManager(env);
  if (!manager) {
    return env.Undefined();
  }
  Napi::ObjectReference mgr = CloneManager(manager_);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    auto owned = manager->GetCatalog(catalog_type_).GetModel(alias);
    return WrapOwnedModelOrUndefined(env, std::move(owned), std::move(mgr), manager, disposed_);
  });
}

Napi::Value Catalog::GetModelVariant(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "getModelVariant(modelId: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string model_id = info[0].As<Napi::String>();
  auto manager = LockManager(env);
  if (!manager) {
    return env.Undefined();
  }
  Napi::ObjectReference mgr = CloneManager(manager_);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    auto owned = manager->GetCatalog(catalog_type_).GetModelVariant(model_id);
    return WrapOwnedModelOrUndefined(env, std::move(owned), std::move(mgr), manager, disposed_);
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
  auto manager = LockManager(env);
  if (!manager) {
    return env.Undefined();
  }
  Napi::ObjectReference mgr = CloneManager(manager_);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    auto owned = manager->GetCatalog(catalog_type_).GetLatestVersion(*arg);
    return WrapOwnedModelOrUndefined(env, std::move(owned), std::move(mgr), manager, disposed_);
  });
}

namespace {

NativeModelInfo* ExtractModelInfo(Napi::Env env, const Napi::Value& value) {
  if (!value.IsObject()) {
    return nullptr;
  }
  Napi::Object object = value.As<Napi::Object>();
  auto* data = env.GetInstanceData<AddonData>();
  if (data == nullptr || !object.InstanceOf(data->model_info_ctor.Value())) {
    return nullptr;
  }
  return Napi::ObjectWrap<NativeModelInfo>::Unwrap(object);
}

bool ReadRegistrationArgs(const Napi::CallbackInfo& info, std::string& model_path, std::string& model_id,
                          std::shared_ptr<foundry_local::ModelInfo>& metadata) {
  Napi::Env env = info.Env();
  if ((info.Length() != 3 && info.Length() != 4) || !info[0].IsString() || !info[1].IsString()) {
    Napi::TypeError::New(env, "registerModel(modelPath: string, modelId: string, metadata: ModelInfo)")
        .ThrowAsJavaScriptException();
    return false;
  }
  NativeModelInfo* wrapper = ExtractModelInfo(env, info[2]);
  if (wrapper == nullptr) {
    Napi::TypeError::New(env, "registerModel: metadata must be a non-disposed ModelInfo")
        .ThrowAsJavaScriptException();
    return false;
  }
  CallCheckedVoid(env, [&]() { metadata = wrapper->Snapshot(); });
  if (env.IsExceptionPending()) {
    return false;
  }
  if (metadata == nullptr) {
    Napi::TypeError::New(env, "registerModel: metadata must be a non-disposed ModelInfo")
        .ThrowAsJavaScriptException();
    return false;
  }
  model_path = info[0].As<Napi::String>();
  model_id = info[1].As<Napi::String>();
  return true;
}

}  // namespace

Napi::Value Catalog::RegisterModel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  std::string model_path;
  std::string model_id;
  std::shared_ptr<foundry_local::ModelInfo> metadata;
  if (!ReadRegistrationArgs(info, model_path, model_id, metadata)) {
    return env.Undefined();
  }

  auto manager = LockManager(env);
  if (!manager) {
    return env.Undefined();
  }
  auto worker_start_gate = ReadWorkerStartGate(info, 3, "Catalog.registerModel.workerStarted", "catalog");
  if (env.IsExceptionPending()) {
    return env.Undefined();
  }
  auto disposed = disposed_;
  auto manager_pin = std::make_shared<Napi::ObjectReference>(CloneManager(manager_));
  return PromiseWorker<std::unique_ptr<foundry_local::IModel>>::Run(
      env,
      [manager, catalog_type = catalog_type_, model_path, model_id, metadata, worker_start_gate]() {
        if (worker_start_gate) {
          worker_start_gate->SignalAndWait();
        }
        return manager->GetCatalog(catalog_type).RegisterModel(model_path, model_id, *metadata);
      },
      [manager_pin, manager_lifetime = std::weak_ptr<foundry_local::Manager>(manager), disposed](
          Napi::Env env, std::unique_ptr<foundry_local::IModel>& model) -> Napi::Value {
        return WrapOwnedModelOrUndefined(env, std::move(model), CloneManager(*manager_pin), manager_lifetime,
                                         disposed);
      },
      Napi::Reference<Napi::Object>::New(info.This().As<Napi::Object>(), 1),
      [disposed]() {
        if (disposed->load()) {
          throw foundry_local::Error("Manager was disposed before registerModel completed",
                                     FOUNDRY_LOCAL_ERROR_INVALID_USAGE);
        }
      });
}

Napi::Value Catalog::RegisterModelSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  std::string model_path;
  std::string model_id;
  std::shared_ptr<foundry_local::ModelInfo> metadata;
  if (!ReadRegistrationArgs(info, model_path, model_id, metadata)) {
    return env.Undefined();
  }
  auto manager_lifetime = LockManager(env);
  if (!manager_lifetime) {
    return env.Undefined();
  }
  Napi::ObjectReference manager = CloneManager(manager_);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    auto model = manager_lifetime->GetCatalog(catalog_type_).RegisterModel(model_path, model_id, *metadata);
    return WrapOwnedModelOrUndefined(env, std::move(model), std::move(manager), manager_lifetime, disposed_);
  });
}

Napi::Value Catalog::UnregisterModel(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if ((info.Length() != 1 && info.Length() != 2) || !info[0].IsString()) {
    Napi::TypeError::New(env, "unregisterModel(aliasOrModelId: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string alias_or_model_id = info[0].As<Napi::String>();
  auto manager = LockManager(env);
  if (!manager) {
    return env.Undefined();
  }
  auto worker_start_gate = ReadWorkerStartGate(info, 1, "Catalog.unregisterModel.workerStarted", "catalog");
  if (env.IsExceptionPending()) {
    return env.Undefined();
  }
  return PromiseWorkerVoid::Run(
      env,
      [manager, catalog_type = catalog_type_, alias_or_model_id, worker_start_gate]() {
        if (worker_start_gate) {
          worker_start_gate->SignalAndWait();
        }
        manager->GetCatalog(catalog_type).UnregisterModel(alias_or_model_id);
      },
      Napi::Reference<Napi::Object>::New(info.This().As<Napi::Object>(), 1));
}

Napi::Value Catalog::UnregisterModelSync(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() != 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "unregisterModelSync(aliasOrModelId: string)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  std::string alias_or_model_id = info[0].As<Napi::String>();
  auto manager = LockManager(env);
  if (!manager) {
    return env.Undefined();
  }
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    manager->GetCatalog(catalog_type_).UnregisterModel(alias_or_model_id);
    return env.Undefined();
  });
}

}  // namespace foundry_local_node
