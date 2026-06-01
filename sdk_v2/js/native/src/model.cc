// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "model.h"

#include "addon_data.h"
#include "errors.h"
#include "promise_worker.h"

#include <foundry_local/foundry_local_c.h>
#include <foundry_local/foundry_local_cpp.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace foundry_local_node {

namespace {

const char* DeviceTypeToString(flDeviceType dt) {
  switch (dt) {
    case FOUNDRY_LOCAL_DEVICE_CPU:
      return "CPU";
    case FOUNDRY_LOCAL_DEVICE_GPU:
      return "GPU";
    case FOUNDRY_LOCAL_DEVICE_NPU:
      return "NPU";
    case FOUNDRY_LOCAL_DEVICE_NOTSET:
    default:
      return "Invalid";
  }
}

// Set `obj[key] = value` from a std::optional<std::string_view>. Omits the
// property when the optional is empty so the JS shape matches `?: T`.
void SetOptionalString(Napi::Env env, Napi::Object obj, const char* key,
                       const std::optional<std::string_view>& value) {
  if (value.has_value()) {
    obj.Set(key, Napi::String::New(env, std::string(*value)));
  }
}

void SetOptionalNumber(Napi::Env env, Napi::Object obj, const char* key,
                       const std::optional<int64_t>& value) {
  if (value.has_value()) {
    obj.Set(key, Napi::Number::New(env, static_cast<double>(*value)));
  }
}

void SetOptionalBool(Napi::Env env, Napi::Object obj, const char* key,
                     const std::optional<bool>& value) {
  if (value.has_value()) {
    obj.Set(key, Napi::Boolean::New(env, *value));
  }
}

void SetPromptTemplate(Napi::Env env, Napi::Object obj, const foundry_local::ModelInfo& info) {
  auto system = info.GetPromptTemplate("system");
  auto user = info.GetPromptTemplate("user");
  auto assistant = info.GetPromptTemplate("assistant");
  auto prompt = info.GetPromptTemplate("prompt");

  if (!system.has_value() && !user.has_value() && !assistant.has_value() && !prompt.has_value()) {
    return;
  }

  Napi::Object template_obj = Napi::Object::New(env);
  SetOptionalString(env, template_obj, "system", system);
  SetOptionalString(env, template_obj, "user", user);
  SetOptionalString(env, template_obj, "assistant", assistant);
  SetOptionalString(env, template_obj, "prompt", prompt);
  obj.Set("promptTemplate", template_obj);
}

void SetModelSettings(Napi::Env env, Napi::Object obj, const foundry_local::ModelInfo& info) {
  auto settings = info.GetModelSettings();
  if (!settings.has_value()) {
    return;
  }

  auto pairs = settings->GetAll();
  if (pairs.empty()) {
    return;
  }

  Napi::Array parameters = Napi::Array::New(env, pairs.size());
  for (size_t i = 0; i < pairs.size(); ++i) {
    Napi::Object parameter = Napi::Object::New(env);
    parameter.Set("name", Napi::String::New(env, std::string(pairs[i].key)));
    parameter.Set("value", Napi::String::New(env, std::string(pairs[i].value)));
    parameters.Set(static_cast<uint32_t>(i), parameter);
  }

  Napi::Object model_settings = Napi::Object::New(env);
  model_settings.Set("parameters", parameters);
  obj.Set("modelSettings", model_settings);
}

Napi::Object SnapshotModelInfo(Napi::Env env, const foundry_local::ModelInfo& info) {
  Napi::Object out = Napi::Object::New(env);

  // Required fields.
  out.Set("id", Napi::String::New(env, std::string(info.Id())));
  out.Set("name", Napi::String::New(env, std::string(info.Name())));
  out.Set("version", Napi::Number::New(env, info.Version()));
  out.Set("alias", Napi::String::New(env, std::string(info.Alias())));
  out.Set("uri", Napi::String::New(env, std::string(info.Uri())));
  out.Set("deviceType", Napi::String::New(env, DeviceTypeToString(info.DeviceType())));
  out.Set("providerType", Napi::String::New(env, std::string(info.ModelProvider().value_or(""))));

  Napi::Object runtime = Napi::Object::New(env);
  runtime.Set("deviceType", Napi::String::New(env, DeviceTypeToString(info.DeviceType())));
  runtime.Set("executionProvider", Napi::String::New(env, std::string(info.ExecutionProvider().value_or(""))));
  out.Set("runtime", runtime);

  out.Set("createdAtUnix", Napi::Number::New(env, static_cast<double>(info.CreatedAtUnix())));
  out.Set("isTestModel", Napi::Boolean::New(env, info.IsTestModel()));

  // Optional fields.
  SetOptionalString(env, out, "executionProvider", info.ExecutionProvider());
  SetOptionalString(env, out, "displayName", info.DisplayName());
  SetOptionalString(env, out, "modelType", info.ModelType());
  SetOptionalString(env, out, "publisher", info.Publisher());
  SetOptionalString(env, out, "license", info.License());
  SetOptionalString(env, out, "licenseDescription", info.LicenseDescription());
  SetOptionalString(env, out, "task", info.Task());
  SetOptionalString(env, out, "modelProvider", info.ModelProvider());
  SetOptionalString(env, out, "minFLVersion", info.MinFlVersion());
  SetOptionalString(env, out, "parentUri", info.ParentUri());
  SetOptionalBool(env, out, "supportsToolCalling", info.SupportsToolCalling());
  SetOptionalNumber(env, out, "fileSizeMb", info.FilesizeMb());
  SetOptionalNumber(env, out, "maxOutputTokens", info.MaxOutputTokens());
  SetOptionalNumber(env, out, "contextLength", info.ContextLength());
  SetOptionalString(env, out, "inputModalities", info.InputModalities());
  SetOptionalString(env, out, "outputModalities", info.OutputModalities());
  SetOptionalString(env, out, "capabilities", info.Capabilities());
  SetPromptTemplate(env, out, info);
  SetModelSettings(env, out, info);

  return out;
}

// Drain a ModelList into a JS array, with each entry wrapped as a JS Model
// whose keepalive holds the shared ModelList.
Napi::Array WrapModelList(Napi::Env env, std::shared_ptr<foundry_local::ModelList> list,
                          Napi::ObjectReference manager) {
  auto models = list->Models();
  Napi::Array arr = Napi::Array::New(env, models.size());
  for (size_t i = 0; i < models.size(); ++i) {
    ModelCtorToken token;
    token.impl = models[i].get();
    token.keepalive = list;  // shared_ptr copy keeps the ModelList alive
    // Cloning the manager ObjectReference per Model so each entry pins it.
    token.manager = Napi::Reference<Napi::Object>::New(manager.Value(), 1);
    arr.Set(static_cast<uint32_t>(i), Model::NewInstance(env, std::move(token)));
  }
  return arr;
}

}  // namespace

Napi::Function Model::Init(Napi::Env env) {
  return DefineClass(env, "Model",
                     {
                         InstanceMethod("getInfo", &Model::GetInfo),
                         InstanceMethod("isCached", &Model::IsCached),
                         InstanceMethod("isLoaded", &Model::IsLoaded),
                         InstanceMethod("getPath", &Model::GetPath),
                         InstanceMethod("getVariants", &Model::GetVariants),
                         InstanceMethod("selectVariant", &Model::SelectVariant),
                         InstanceMethod("load", &Model::Load),
                         InstanceMethod("unload", &Model::Unload),
                         InstanceMethod("download", &Model::Download),
                         InstanceMethod("removeFromCache", &Model::RemoveFromCache),
                     });
}

Napi::Object Model::NewInstance(Napi::Env env, ModelCtorToken token) {
  auto* heap = new ModelCtorToken(std::move(token));
  auto ext = Napi::External<ModelCtorToken>::New(
      env, heap, [](Napi::Env /*env*/, ModelCtorToken* t) { delete t; });
  auto* data = env.GetInstanceData<AddonData>();
  return data->model_ctor.New({ext});
}

Model::Model(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Model>(info) {
  Napi::Env env = info.Env();
  if (info.Length() != 1 || !info[0].IsExternal()) {
    Napi::TypeError::New(env, "Model is internal — obtain instances via Catalog/Manager methods")
        .ThrowAsJavaScriptException();
    return;
  }
  auto* token = info[0].As<Napi::External<ModelCtorToken>>().Data();
  if (token == nullptr || token->impl == nullptr) {
    Napi::TypeError::New(env, "Model: invalid internal construction token").ThrowAsJavaScriptException();
    return;
  }
  impl_ = token->impl;
  keepalive_ = std::move(token->keepalive);
  manager_ = std::move(token->manager);
}

Napi::Value Model::GetInfo(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    foundry_local::ModelInfo mi = impl_->GetInfo();
    Napi::Object snapshot = SnapshotModelInfo(env, mi);
    snapshot.Set("cached", Napi::Boolean::New(env, impl_->IsCached()));
    return snapshot;
  });
}

Napi::Value Model::IsCached(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    return Napi::Boolean::New(env, impl_->IsCached());
  });
}

Napi::Value Model::IsLoaded(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    return Napi::Boolean::New(env, impl_->IsLoaded());
  });
}

Napi::Value Model::GetPath(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    std::string_view p = impl_->GetPath();
    return Napi::String::New(env, std::string(p));
  });
}

Napi::Value Model::GetVariants(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::ObjectReference owner_clone = Napi::Reference<Napi::Object>::New(manager_.Value(), 1);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    auto list = std::make_shared<foundry_local::ModelList>(impl_->GetVariants());
    return WrapModelList(env, std::move(list), std::move(owner_clone));
  });
}

// ── Async lifecycle ─────────────────────────────────────────────────────────
//
// Load/Unload/Download dispatch the underlying virtual call onto a libuv
// worker so the event loop stays responsive. The Model itself is pinned
// against GC for the duration of the worker via an ObjectReference to the
// parent Manager (the Manager owns the catalog whose ModelList views the
// IModel*).

Napi::Value Model::Load(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (impl_ == nullptr) {
    Napi::Error::New(env, "Model: not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::ObjectReference owner = Napi::Reference<Napi::Object>::New(manager_.Value(), 1);
  foundry_local::IModel* m = impl_;
  return PromiseWorkerVoid::Run(
      env, [m]() { m->Load(); }, std::move(owner));
}

Napi::Value Model::Unload(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (impl_ == nullptr) {
    Napi::Error::New(env, "Model: not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::ObjectReference owner = Napi::Reference<Napi::Object>::New(manager_.Value(), 1);
  foundry_local::IModel* m = impl_;
  return PromiseWorkerVoid::Run(
      env, [m]() { m->Unload(); }, std::move(owner));
}

namespace {

// AsyncWorker variant that drives IModel::Download with an optional JS
// progress callback. The callback runs on the libuv worker thread; we bounce
// each (float percent) to JS via a ThreadSafeFunction acquired before the
// worker queues and released in OnOK/OnError.
class DownloadWorker : public Napi::AsyncWorker {
 public:
  DownloadWorker(Napi::Env env, foundry_local::IModel* impl, Napi::ObjectReference owner,
                 Napi::ThreadSafeFunction tsfn)
      : Napi::AsyncWorker(env),
        deferred_(Napi::Promise::Deferred::New(env)),
        impl_(impl),
        owner_(std::move(owner)),
        tsfn_(std::move(tsfn)) {}

  Napi::Promise Promise() { return deferred_.Promise(); }

  void Execute() override {
    try {
      auto progress_cb = tsfn_ ? std::function<int(float)>([this](float percent) {
        // BlockingCall keeps backpressure on the worker thread: if JS is
        // slow to drain the queue we'll wait rather than dropping reports.
        // Callback return value is unused on the JS side; we always continue.
        tsfn_.BlockingCall([percent](Napi::Env env, Napi::Function js_cb) {
          js_cb.Call({Napi::Number::New(env, static_cast<double>(percent))});
        });
        return 0;  // 0 = continue per flProgressCallback contract.
      })
                               : std::function<int(float)>(nullptr);
      impl_->Download(std::move(progress_cb));
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
    Napi::HandleScope scope(Env());
    ReleaseTsfn();
    deferred_.Resolve(Env().Undefined());
  }

  void OnError(const Napi::Error& /*unused*/) override {
    Napi::Env env = Env();
    Napi::HandleScope scope(env);
    ReleaseTsfn();
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
  void ReleaseTsfn() {
    if (tsfn_) {
      tsfn_.Release();
      tsfn_ = Napi::ThreadSafeFunction();
    }
  }

  Napi::Promise::Deferred deferred_;
  foundry_local::IModel* impl_;
  Napi::ObjectReference owner_;
  Napi::ThreadSafeFunction tsfn_;
  std::string err_msg_;
  int err_code_ = 0;
  bool tagged_ = false;
};

}  // namespace

Napi::Value Model::Download(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (impl_ == nullptr) {
    Napi::Error::New(env, "Model: not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::ThreadSafeFunction tsfn;
  if (info.Length() >= 1 && info[0].IsFunction()) {
    tsfn = Napi::ThreadSafeFunction::New(env, info[0].As<Napi::Function>(),
                                         "Model.download.progress",
                                         /*max_queue_size=*/0,
                                         /*initial_thread_count=*/1);
  } else if (info.Length() >= 1 && !info[0].IsUndefined() && !info[0].IsNull()) {
    Napi::TypeError::New(env, "Model.download: progress callback must be a function")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::ObjectReference owner = Napi::Reference<Napi::Object>::New(manager_.Value(), 1);
  auto* w = new DownloadWorker(env, impl_, std::move(owner), std::move(tsfn));
  Napi::Promise p = w->Promise();
  w->Queue();
  return p;
}

Napi::Value Model::RemoveFromCache(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (impl_ == nullptr) {
    Napi::Error::New(env, "Model: not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  // Sync — the underlying RemoveFromCache is a fast filesystem cleanup;
  // V1's contract is `removeFromCache(): void` so we do not bounce to a
  // worker.
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    impl_->RemoveFromCache();
    return env.Undefined();
  });
}

Napi::Value Model::SelectVariant(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (impl_ == nullptr) {
    Napi::Error::New(env, "Model: not initialized").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto* data = env.GetInstanceData<AddonData>();
  if (info.Length() != 1 || !info[0].IsObject() ||
      !info[0].As<Napi::Object>().InstanceOf(data->model_ctor.Value())) {
    Napi::TypeError::New(env, "Model.selectVariant: expected a Model instance").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Model* variant = Napi::ObjectWrap<Model>::Unwrap(info[0].As<Napi::Object>());
  if (variant == nullptr || variant->impl_ == nullptr) {
    Napi::TypeError::New(env, "Model.selectVariant: variant Model is not initialized")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    impl_->SelectVariant(*variant->impl_);
    return env.Undefined();
  });
}

}  // namespace foundry_local_node
