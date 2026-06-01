// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "request_options.h"

#include <foundry_local/foundry_local_c.h>

#include <string>

namespace foundry_local_node {

namespace {

bool IsUnsetValue(const Napi::Value& v) {
  return v.IsUndefined() || v.IsNull();
}

std::optional<float> GetOptionalFloat(Napi::Env env, const Napi::Object& obj, const char* key) {
  if (!obj.Has(key)) {
    return std::nullopt;
  }
  Napi::Value v = obj.Get(key);
  if (IsUnsetValue(v)) {
    return std::nullopt;
  }
  if (!v.IsNumber()) {
    throw Napi::TypeError::New(env, std::string("search.") + key + ": expected a number");
  }
  return static_cast<float>(v.As<Napi::Number>().DoubleValue());
}

std::optional<int> GetOptionalInt(Napi::Env env, const Napi::Object& obj, const char* key) {
  if (!obj.Has(key)) {
    return std::nullopt;
  }
  Napi::Value v = obj.Get(key);
  if (IsUnsetValue(v)) {
    return std::nullopt;
  }
  if (!v.IsNumber()) {
    throw Napi::TypeError::New(env, std::string("search.") + key + ": expected a number");
  }
  return v.As<Napi::Number>().Int32Value();
}

std::optional<bool> GetOptionalBool(Napi::Env env, const Napi::Object& obj, const char* key) {
  if (!obj.Has(key)) {
    return std::nullopt;
  }
  Napi::Value v = obj.Get(key);
  if (IsUnsetValue(v)) {
    return std::nullopt;
  }
  if (!v.IsBoolean()) {
    throw Napi::TypeError::New(env, std::string("search.") + key + ": expected a boolean");
  }
  return v.As<Napi::Boolean>().Value();
}

foundry_local::SearchOptions JsToSearchOptions(Napi::Env env, const Napi::Object& search) {
  foundry_local::SearchOptions s;
  s.temperature = GetOptionalFloat(env, search, "temperature");
  s.top_p = GetOptionalFloat(env, search, "topP");
  s.top_k = GetOptionalInt(env, search, "topK");
  s.max_output_tokens = GetOptionalInt(env, search, "maxOutputTokens");
  s.frequency_penalty = GetOptionalFloat(env, search, "frequencyPenalty");
  s.presence_penalty = GetOptionalFloat(env, search, "presencePenalty");
  s.seed = GetOptionalInt(env, search, "seed");
  s.early_stopping = GetOptionalBool(env, search, "earlyStopping");
  s.do_sample = GetOptionalBool(env, search, "doSample");
  return s;
}

flToolChoice ParseToolChoice(Napi::Env env, const std::string& s) {
  if (s == "auto") return FOUNDRY_LOCAL_TOOL_CHOICE_AUTO;
  if (s == "none") return FOUNDRY_LOCAL_TOOL_CHOICE_NONE;
  if (s == "required") return FOUNDRY_LOCAL_TOOL_CHOICE_REQUIRED;
  throw Napi::TypeError::New(
      env, "toolChoice: expected 'auto' | 'none' | 'required', got '" + s + "'");
}

foundry_local::KeyValuePairs JsToAdditionalOptions(Napi::Env env, const Napi::Object& obj) {
  foundry_local::KeyValuePairs kvp;
  Napi::Array keys = obj.GetPropertyNames();
  for (uint32_t i = 0; i < keys.Length(); ++i) {
    Napi::Value k = keys.Get(i);
    if (!k.IsString()) continue;
    std::string key = k.As<Napi::String>().Utf8Value();
    Napi::Value v = obj.Get(k);
    if (IsUnsetValue(v)) continue;
    std::string value;
    if (v.IsString()) {
      value = v.As<Napi::String>().Utf8Value();
    } else if (v.IsNumber()) {
      value = v.ToString().Utf8Value();
    } else if (v.IsBoolean()) {
      value = v.As<Napi::Boolean>().Value() ? "true" : "false";
    } else {
      throw Napi::TypeError::New(
          env, "additionalOptions['" + key + "']: must be string, number, or boolean");
    }
    kvp.Set(key.c_str(), value.c_str());
  }
  return kvp;
}

}  // namespace

foundry_local::RequestOptions JsToRequestOptions(Napi::Env env, const Napi::Object& opts) {
  foundry_local::RequestOptions out;

  if (opts.Has("search")) {
    Napi::Value v = opts.Get("search");
    if (!IsUnsetValue(v)) {
      if (!v.IsObject()) {
        throw Napi::TypeError::New(env, "setOptions: 'search' must be an object");
      }
      out.search = JsToSearchOptions(env, v.As<Napi::Object>());
    }
  }

  if (opts.Has("toolChoice")) {
    Napi::Value v = opts.Get("toolChoice");
    if (!IsUnsetValue(v)) {
      if (!v.IsString()) {
        throw Napi::TypeError::New(env, "setOptions: 'toolChoice' must be a string");
      }
      out.tool_choice = ParseToolChoice(env, v.As<Napi::String>().Utf8Value());
    }
  }

  if (opts.Has("additionalOptions")) {
    Napi::Value v = opts.Get("additionalOptions");
    if (!IsUnsetValue(v)) {
      if (!v.IsObject()) {
        throw Napi::TypeError::New(env, "setOptions: 'additionalOptions' must be an object");
      }
      out.additional_options = JsToAdditionalOptions(env, v.As<Napi::Object>());
    }
  }

  return out;
}

}  // namespace foundry_local_node
