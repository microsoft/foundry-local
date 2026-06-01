// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "manager.h"

#include "addon_data.h"
#include "catalog.h"
#include "errors.h"
#include "promise_worker.h"

#include <foundry_local/foundry_local_c.h>
#include <foundry_local/foundry_local_cpp.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace foundry_local_node {

namespace {

Napi::Value ConvertEndpoints(Napi::Env env, std::vector<std::string>& endpoints) {
  Napi::Array out = Napi::Array::New(env, endpoints.size());
  for (size_t i = 0; i < endpoints.size(); ++i) {
    out.Set(static_cast<uint32_t>(i), Napi::String::New(env, endpoints[i]));
  }
  return out;
}

// Build a tagged FoundryLocalError pending on env. Mirrors errors.cc's
// internal `MakeFoundryLocalError` — duplicated here to avoid exposing the
// helper publicly just for the disposed-manager path.
void ThrowFoundryLocalError(Napi::Env env, int code, const std::string& msg) {
  Napi::Error err = Napi::Error::New(env, msg);
  Napi::Object value = err.Value();
  value.Set("name", Napi::String::New(env, "FoundryLocalError"));
  value.Set("code", Napi::Number::New(env, code));
  err.ThrowAsJavaScriptException();
}

}  // namespace

Napi::Function Manager::Init(Napi::Env env) {
  return DefineClass(env, "Manager",
                     {
                         InstanceMethod("getWebServiceEndpoints", &Manager::GetWebServiceEndpoints),
                         InstanceMethod("getCatalog", &Manager::GetCatalog),
                         InstanceMethod("startWebService", &Manager::StartWebService),
                         InstanceMethod("stopWebService", &Manager::StopWebService),
                         InstanceMethod("discoverEps", &Manager::DiscoverEps),
                         InstanceMethod("downloadAndRegisterEps", &Manager::DownloadAndRegisterEps),
                         InstanceMethod("isEpDownloadInProgress", &Manager::IsEpDownloadInProgress),
                         InstanceMethod("shutdown", &Manager::Shutdown),
                         InstanceMethod("isShutdownRequested", &Manager::IsShutdownRequested),
                         InstanceMethod("dispose", &Manager::Dispose),
                         InstanceMethod("isDisposed", &Manager::IsDisposed),
                     });
}

Manager::Manager(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Manager>(info) {
  Napi::Env env = info.Env();

  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "Manager constructor expects an options object").ThrowAsJavaScriptException();
    return;
  }
  Napi::Object opts = info[0].As<Napi::Object>();
  if (!opts.Has("appName") || !opts.Get("appName").IsString()) {
    Napi::TypeError::New(env, "options.appName must be a string").ThrowAsJavaScriptException();
    return;
  }
  std::string app_name = opts.Get("appName").As<Napi::String>();

  // Optional Configuration setters — type-checked when present.
  auto read_optional_string = [&](const char* key, std::string& out, bool& has) -> bool {
    if (opts.Has(key) && !opts.Get(key).IsUndefined() && !opts.Get(key).IsNull()) {
      if (!opts.Get(key).IsString()) {
        std::string msg = "options.";
        msg += key;
        msg += " must be a string";
        Napi::TypeError::New(env, msg).ThrowAsJavaScriptException();
        return false;
      }
      out = opts.Get(key).As<Napi::String>();
      has = true;
    }
    return true;
  };

  std::string model_cache_dir;
  bool has_model_cache_dir = false;
  if (!read_optional_string("modelCacheDir", model_cache_dir, has_model_cache_dir)) return;

  std::string external_service_url;
  bool has_external_service_url = false;
  if (!read_optional_string("serviceEndpoint", external_service_url, has_external_service_url)) return;

  std::string app_data_dir;
  bool has_app_data_dir = false;
  if (!read_optional_string("appDataDir", app_data_dir, has_app_data_dir)) return;

  std::string logs_dir;
  bool has_logs_dir = false;
  if (!read_optional_string("logsDir", logs_dir, has_logs_dir)) return;

  std::string web_service_url;
  bool has_web_service_url = false;
  if (!read_optional_string("webServiceUrls", web_service_url, has_web_service_url)) return;

  // additionalSettings: optional plain object whose own enumerable properties
  // must all have string values. Empty object is equivalent to omitting the
  // field. Forwarded to the C++ Configuration via SetAdditionalOptions.
  std::vector<std::pair<std::string, std::string>> additional_settings;
  bool has_additional_settings = false;
  if (opts.Has("additionalSettings") && !opts.Get("additionalSettings").IsUndefined() &&
      !opts.Get("additionalSettings").IsNull()) {
    if (!opts.Get("additionalSettings").IsObject()) {
      Napi::TypeError::New(env, "options.additionalSettings must be an object").ThrowAsJavaScriptException();
      return;
    }
    Napi::Object kv = opts.Get("additionalSettings").As<Napi::Object>();
    Napi::Array keys = kv.GetPropertyNames();
    for (uint32_t i = 0; i < keys.Length(); ++i) {
      Napi::Value k = keys.Get(i);
      if (!k.IsString()) continue;
      std::string key = k.As<Napi::String>();
      Napi::Value v = kv.Get(k);
      if (!v.IsString()) {
        std::string msg = "options.additionalSettings[";
        msg += key;
        msg += "] must be a string";
        Napi::TypeError::New(env, msg).ThrowAsJavaScriptException();
        return;
      }
      additional_settings.emplace_back(std::move(key), v.As<Napi::String>().Utf8Value());
    }
    has_additional_settings = !additional_settings.empty();
  }

  flLogLevel log_level = FOUNDRY_LOCAL_LOG_INFO;
  bool has_log_level = false;
  if (opts.Has("logLevel") && !opts.Get("logLevel").IsUndefined() && !opts.Get("logLevel").IsNull()) {
    if (!opts.Get("logLevel").IsString()) {
      Napi::TypeError::New(env, "options.logLevel must be a string").ThrowAsJavaScriptException();
      return;
    }
    std::string lvl = opts.Get("logLevel").As<Napi::String>();
    if (lvl == "trace")
      log_level = FOUNDRY_LOCAL_LOG_VERBOSE;
    else if (lvl == "debug")
      log_level = FOUNDRY_LOCAL_LOG_DEBUG;
    else if (lvl == "info")
      log_level = FOUNDRY_LOCAL_LOG_INFO;
    else if (lvl == "warn")
      log_level = FOUNDRY_LOCAL_LOG_WARNING;
    else if (lvl == "error")
      log_level = FOUNDRY_LOCAL_LOG_ERROR;
    else if (lvl == "fatal")
      log_level = FOUNDRY_LOCAL_LOG_FATAL;
    else {
      Napi::TypeError::New(env, "options.logLevel must be one of trace|debug|info|warn|error|fatal")
          .ThrowAsJavaScriptException();
      return;
    }
    has_log_level = true;
  }

  CallCheckedVoid(env, [&]() {
    foundry_local::Configuration config(app_name);
    if (has_model_cache_dir) {
      config.SetModelCacheDir(model_cache_dir);
    }
    if (has_external_service_url) {
      config.SetExternalServiceUrl(external_service_url);
    }
    if (has_app_data_dir) {
      config.SetAppDataDir(app_data_dir);
    }
    if (has_logs_dir) {
      config.SetLogsDir(logs_dir);
    }
    if (has_log_level) {
      config.SetDefaultLogLevel(log_level);
    }
    if (has_web_service_url) {
      config.AddWebServiceEndpoint(web_service_url);
    }
    if (has_additional_settings) {
      foundry_local::KeyValuePairs kvp;
      for (const auto& entry : additional_settings) {
        kvp.Set(entry.first.c_str(), entry.second.c_str());
      }
      config.SetAdditionalOptions(kvp);
    }
    impl_ = std::make_unique<foundry_local::Manager>(std::move(config));
  });
}

bool Manager::ThrowIfDisposed(Napi::Env env) {
  if (impl_ == nullptr) {
    ThrowFoundryLocalError(env, FOUNDRY_LOCAL_ERROR_INVALID_USAGE, "Manager has been disposed");
    return true;
  }
  return false;
}

Napi::Value Manager::GetWebServiceEndpoints(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) {
    return env.Undefined();
  }
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    std::vector<std::string> endpoints = impl_->GetWebServiceEndpoints();
    return ConvertEndpoints(env, endpoints);
  });
}

Napi::Value Manager::GetCatalog(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) {
    return env.Undefined();
  }
  Napi::ObjectReference owner = Napi::Reference<Napi::Object>::New(info.This().As<Napi::Object>(), 1);
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    foundry_local::ICatalog& cat = impl_->GetCatalog();
    CatalogCtorToken token;
    token.impl = &cat;
    token.manager = std::move(owner);
    return Catalog::NewInstance(env, std::move(token));
  });
}

Napi::Value Manager::Dispose(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  // Idempotent — releasing an already-null unique_ptr is a no-op.
  impl_.reset();
  return env.Undefined();
}

Napi::Value Manager::IsDisposed(const Napi::CallbackInfo& info) {
  return Napi::Boolean::New(info.Env(), impl_ == nullptr);
}

Napi::Value Manager::StartWebService(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) {
    return env.Undefined();
  }
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    impl_->StartWebService();
    return env.Undefined();
  });
}

Napi::Value Manager::StopWebService(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) {
    return env.Undefined();
  }
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    impl_->StopWebService();
    return env.Undefined();
  });
}

Napi::Value Manager::DiscoverEps(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) {
    return env.Undefined();
  }
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    std::vector<foundry_local::EpInfo> eps = impl_->GetDiscoverableEps();
    Napi::Array arr = Napi::Array::New(env, eps.size());
    for (size_t i = 0; i < eps.size(); ++i) {
      Napi::Object obj = Napi::Object::New(env);
      obj.Set("name", Napi::String::New(env, eps[i].name));
      obj.Set("isRegistered", Napi::Boolean::New(env, eps[i].is_registered));
      arr.Set(static_cast<uint32_t>(i), obj);
    }
    return arr;
  });
}

namespace {

// AsyncWorker for DownloadAndRegisterEps with optional (epName, percent)
// progress callback. Mirrors the pattern in model.cc's DownloadWorker.
class EpDownloadWorker : public Napi::AsyncWorker {
 public:
  EpDownloadWorker(Napi::Env env, foundry_local::Manager* impl, std::vector<std::string> ep_names,
                   Napi::ObjectReference owner, Napi::ThreadSafeFunction tsfn)
      : Napi::AsyncWorker(env),
        deferred_(Napi::Promise::Deferred::New(env)),
        impl_(impl),
        ep_names_(std::move(ep_names)),
        owner_(std::move(owner)),
        tsfn_(std::move(tsfn)) {}

  Napi::Promise Promise() { return deferred_.Promise(); }

  void Execute() override {
    try {
      std::function<bool(std::string_view, float)> progress_cb;
      if (tsfn_) {
        progress_cb = [this](std::string_view ep_name, float percent) -> bool {
          std::string name(ep_name);
          tsfn_.BlockingCall([name, percent](Napi::Env env, Napi::Function js_cb) {
            js_cb.Call({Napi::String::New(env, name), Napi::Number::New(env, static_cast<double>(percent))});
          });
          return true;  // continue
        };
      }
      impl_->DownloadAndRegisterEps(ep_names_, std::move(progress_cb));
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
  foundry_local::Manager* impl_;
  std::vector<std::string> ep_names_;
  Napi::ObjectReference owner_;
  Napi::ThreadSafeFunction tsfn_;
  std::string err_msg_;
  int err_code_ = 0;
  bool tagged_ = false;
};

}  // namespace

Napi::Value Manager::DownloadAndRegisterEps(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) {
    return env.Undefined();
  }

  // Args: (names?: string[], onProgress?: (epName: string, percent: number) => void)
  std::vector<std::string> ep_names;
  Napi::Function js_cb;
  bool has_cb = false;

  if (info.Length() >= 1 && !info[0].IsUndefined() && !info[0].IsNull()) {
    if (!info[0].IsArray()) {
      Napi::TypeError::New(env, "Manager.downloadAndRegisterEps: names must be an array of strings")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    Napi::Array arr = info[0].As<Napi::Array>();
    for (uint32_t i = 0; i < arr.Length(); ++i) {
      Napi::Value v = arr.Get(i);
      if (!v.IsString()) {
        Napi::TypeError::New(env, "Manager.downloadAndRegisterEps: names entries must be strings")
            .ThrowAsJavaScriptException();
        return env.Undefined();
      }
      ep_names.emplace_back(v.As<Napi::String>().Utf8Value());
    }
  }
  if (info.Length() >= 2 && !info[1].IsUndefined() && !info[1].IsNull()) {
    if (!info[1].IsFunction()) {
      Napi::TypeError::New(env, "Manager.downloadAndRegisterEps: onProgress must be a function")
          .ThrowAsJavaScriptException();
      return env.Undefined();
    }
    js_cb = info[1].As<Napi::Function>();
    has_cb = true;
  }

  Napi::ThreadSafeFunction tsfn;
  if (has_cb) {
    tsfn = Napi::ThreadSafeFunction::New(env, js_cb, "Manager.downloadAndRegisterEps.progress",
                                         /*max_queue_size=*/0,
                                         /*initial_thread_count=*/1);
  }

  Napi::ObjectReference owner = Napi::Reference<Napi::Object>::New(info.This().As<Napi::Object>(), 1);
  auto* w = new EpDownloadWorker(env, impl_.get(), std::move(ep_names), std::move(owner), std::move(tsfn));
  Napi::Promise p = w->Promise();
  w->Queue();
  return p;
}

Napi::Value Manager::IsEpDownloadInProgress(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) {
    return env.Undefined();
  }
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    return Napi::Boolean::New(env, impl_->IsEpDownloadInProgress());
  });
}

Napi::Value Manager::Shutdown(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) {
    return env.Undefined();
  }
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    impl_->Shutdown();
    return env.Undefined();
  });
}

Napi::Value Manager::IsShutdownRequested(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (ThrowIfDisposed(env)) {
    return env.Undefined();
  }
  return CallChecked<Napi::Value>(env, [&]() -> Napi::Value {
    return Napi::Boolean::New(env, impl_->IsShutdownRequested());
  });
}

}  // namespace foundry_local_node
