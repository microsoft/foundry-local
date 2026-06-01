// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "items.h"

#include "addon_data.h"

#include <foundry_local/foundry_local_c.h>
#include <foundry_local/foundry_local_cpp.h>

#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace foundry_local_node {

namespace {

// ── Enum string mappings (kept stable across the JS surface) ───────────────

const char* RoleToString(flMessageRole r) {
  switch (r) {
    case FOUNDRY_LOCAL_ROLE_SYSTEM:
      return "system";
    case FOUNDRY_LOCAL_ROLE_USER:
      return "user";
    case FOUNDRY_LOCAL_ROLE_ASSISTANT:
      return "assistant";
    case FOUNDRY_LOCAL_ROLE_TOOL:
      return "tool";
    case FOUNDRY_LOCAL_ROLE_DEVELOPER:
      return "developer";
    case FOUNDRY_LOCAL_ROLE_NONE:
    default:
      return "none";
  }
}

bool RoleFromString(const std::string& s, flMessageRole& out) {
  if (s == "system") {
    out = FOUNDRY_LOCAL_ROLE_SYSTEM;
    return true;
  }
  if (s == "user") {
    out = FOUNDRY_LOCAL_ROLE_USER;
    return true;
  }
  if (s == "assistant") {
    out = FOUNDRY_LOCAL_ROLE_ASSISTANT;
    return true;
  }
  if (s == "tool") {
    out = FOUNDRY_LOCAL_ROLE_TOOL;
    return true;
  }
  if (s == "developer") {
    out = FOUNDRY_LOCAL_ROLE_DEVELOPER;
    return true;
  }
  return false;
}

const char* TextTypeToString(flTextItemType t) {
  switch (t) {
    case FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING:
      return "reasoning";
    case FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON:
      return "openai-json";
    case FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT:
    default:
      return "default";
  }
}

bool TextTypeFromString(const std::string& s, flTextItemType& out) {
  if (s == "default") {
    out = FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT;
    return true;
  }
  if (s == "reasoning") {
    out = FOUNDRY_LOCAL_TEXT_ITEM_TYPE_REASONING;
    return true;
  }
  if (s == "openai-json") {
    out = FOUNDRY_LOCAL_TEXT_ITEM_TYPE_OPENAI_JSON;
    return true;
  }
  return false;
}

const char* ItemTypeToString(flItemType t) {
  switch (t) {
    case FOUNDRY_LOCAL_ITEM_BYTES:
      return "bytes";
    case FOUNDRY_LOCAL_ITEM_TENSOR:
      return "tensor";
    case FOUNDRY_LOCAL_ITEM_TEXT:
      return "text";
    case FOUNDRY_LOCAL_ITEM_MESSAGE:
      return "message";
    case FOUNDRY_LOCAL_ITEM_IMAGE:
      return "image";
    case FOUNDRY_LOCAL_ITEM_AUDIO:
      return "audio";
    case FOUNDRY_LOCAL_ITEM_TOOL_CALL:
      return "toolCall";
    case FOUNDRY_LOCAL_ITEM_TOOL_RESULT:
      return "toolResult";
    case FOUNDRY_LOCAL_ITEM_QUEUE:
      return "queue";
    case FOUNDRY_LOCAL_ITEM_UNKNOWN:
    default:
      return "unknown";
  }
}

const char* TensorDataTypeToString(flTensorDataType t) {
  switch (t) {
    case FOUNDRY_LOCAL_TENSOR_FLOAT:
      return "float";
    case FOUNDRY_LOCAL_TENSOR_UINT8:
      return "uint8";
    case FOUNDRY_LOCAL_TENSOR_INT8:
      return "int8";
    case FOUNDRY_LOCAL_TENSOR_UINT16:
      return "uint16";
    case FOUNDRY_LOCAL_TENSOR_INT16:
      return "int16";
    case FOUNDRY_LOCAL_TENSOR_INT32:
      return "int32";
    case FOUNDRY_LOCAL_TENSOR_INT64:
      return "int64";
    case FOUNDRY_LOCAL_TENSOR_STRING:
      return "string";
    case FOUNDRY_LOCAL_TENSOR_BOOL:
      return "bool";
    case FOUNDRY_LOCAL_TENSOR_FLOAT16:
      return "float16";
    case FOUNDRY_LOCAL_TENSOR_DOUBLE:
      return "double";
    case FOUNDRY_LOCAL_TENSOR_UINT32:
      return "uint32";
    case FOUNDRY_LOCAL_TENSOR_UINT64:
      return "uint64";
    default:
      return "unknown";
  }
}

[[noreturn]] void ThrowShape(Napi::Env env, const std::string& msg);

// Copy raw bytes from a C buffer into a fresh JS Buffer (owned by JS GC).
Napi::Value BufferCopy(Napi::Env env, const void* data, size_t size) {
  if (data == nullptr || size == 0) {
    return Napi::Buffer<uint8_t>::New(env, 0);
  }
  return Napi::Buffer<uint8_t>::Copy(env, static_cast<const uint8_t*>(data), size);
}

// Byte size per tensor element. 0 for non-portable / opaque types.
size_t TensorElemSize(flTensorDataType t) {
  switch (t) {
    case FOUNDRY_LOCAL_TENSOR_FLOAT:
    case FOUNDRY_LOCAL_TENSOR_INT32:
    case FOUNDRY_LOCAL_TENSOR_UINT32:
      return 4;
    case FOUNDRY_LOCAL_TENSOR_UINT8:
    case FOUNDRY_LOCAL_TENSOR_INT8:
    case FOUNDRY_LOCAL_TENSOR_BOOL:
      return 1;
    case FOUNDRY_LOCAL_TENSOR_UINT16:
    case FOUNDRY_LOCAL_TENSOR_INT16:
    case FOUNDRY_LOCAL_TENSOR_FLOAT16:
      return 2;
    case FOUNDRY_LOCAL_TENSOR_INT64:
    case FOUNDRY_LOCAL_TENSOR_UINT64:
    case FOUNDRY_LOCAL_TENSOR_DOUBLE:
      return 8;
    default:
      return 0;
  }
}

bool TensorDataTypeFromString(const std::string& s, flTensorDataType& out) {
  if (s == "float") {
    out = FOUNDRY_LOCAL_TENSOR_FLOAT;
    return true;
  }
  if (s == "uint8") {
    out = FOUNDRY_LOCAL_TENSOR_UINT8;
    return true;
  }
  if (s == "int8") {
    out = FOUNDRY_LOCAL_TENSOR_INT8;
    return true;
  }
  if (s == "uint16") {
    out = FOUNDRY_LOCAL_TENSOR_UINT16;
    return true;
  }
  if (s == "int16") {
    out = FOUNDRY_LOCAL_TENSOR_INT16;
    return true;
  }
  if (s == "int32") {
    out = FOUNDRY_LOCAL_TENSOR_INT32;
    return true;
  }
  if (s == "int64") {
    out = FOUNDRY_LOCAL_TENSOR_INT64;
    return true;
  }
  if (s == "bool") {
    out = FOUNDRY_LOCAL_TENSOR_BOOL;
    return true;
  }
  if (s == "float16") {
    out = FOUNDRY_LOCAL_TENSOR_FLOAT16;
    return true;
  }
  if (s == "double") {
    out = FOUNDRY_LOCAL_TENSOR_DOUBLE;
    return true;
  }
  if (s == "uint32") {
    out = FOUNDRY_LOCAL_TENSOR_UINT32;
    return true;
  }
  if (s == "uint64") {
    out = FOUNDRY_LOCAL_TENSOR_UINT64;
    return true;
  }
  return false;
}

// Reads `key` as a Uint8Array / Buffer / ArrayBuffer / ArrayBufferView and
// returns a (data, size) view into the underlying memory. The view is only
// valid until the JS object is moved/finalized; raw-bytes call sites pin
// the source via MakePinnedDeleter so the view stays valid for the Item's
// full lifetime. Returns {nullptr, 0} when the field is missing or null.
struct BytesView {
  const uint8_t* data;
  size_t size;
};

// Throws TypeError if `v` is not an accepted byte-buffer shape.
BytesView ParseBytesValue(Napi::Env env, const Napi::Value& v, const char* item_type_tag,
                          const char* key) {
  if (v.IsBuffer()) {
    auto buf = v.As<Napi::Buffer<uint8_t>>();
    return {buf.Data(), buf.Length()};
  }
  if (v.IsTypedArray()) {
    auto ta = v.As<Napi::TypedArray>();
    Napi::ArrayBuffer ab = ta.ArrayBuffer();
    auto* base = static_cast<uint8_t*>(ab.Data());
    return {base + ta.ByteOffset(), ta.ByteLength()};
  }
  if (v.IsArrayBuffer()) {
    auto ab = v.As<Napi::ArrayBuffer>();
    return {static_cast<uint8_t*>(ab.Data()), ab.ByteLength()};
  }
  ThrowShape(env, std::string("Item[type=") + item_type_tag + "].'" + key +
                      "' must be a Uint8Array, Buffer, or ArrayBuffer");
}

// Returns the data-field Value if present and non-null, or std::nullopt.
// Does no type validation — the caller pairs this with ParseBytesValue.
std::optional<Napi::Value> OptBytesValue(const Napi::Object& obj, const char* key) {
  if (!obj.Has(key)) return std::nullopt;
  Napi::Value v = obj.Get(key);
  if (v.IsUndefined() || v.IsNull()) return std::nullopt;
  return v;
}

// ── Pinned-buffer ownership for raw-bytes Item inputs ──────────────────────
//
// Zero-copy path: the addon takes a strong N-API reference to the caller's
// Uint8Array / Buffer, hands the raw pointer + length to the C++ wrapper
// Item, and releases the reference from the Item's deleter. The buffer is
// pinned for the Item's full lifetime (Request -> Session -> inference) and
// the JS GC cannot reclaim it.
//
// The deleter must call napi_delete_reference on the JS thread. Today
// Request-owned items destruct on the JS thread (ObjectWrap finalizer), so
// a direct call would work — but once ItemQueue lands the deleter may fire
// on a native consumer thread, so we always bounce through the shared
// per-addon TSFN. Single path; no thread-of-call branching.
struct PinnedBuffer {
  napi_env env;
  napi_ref ref;
};

void ReleasePinnedOnJs(Napi::Env env, Napi::Function /*noop*/, PinnedBuffer* p) {
  napi_delete_reference(env, p->ref);
  delete p;
}

// Builds a deleter parameterised on the C wrapper's payload struct type
// (flBytesData / flTensorData / flImageData / flAudioData). The deleter
// ignores its argument — the C wrapper only passes it so callers that
// allocated alongside the payload can recover context, which we don't need.
template <typename T>
std::function<void(const T*)> MakePinnedDeleter(Napi::Env env, Napi::Value source) {
  auto* data = env.GetInstanceData<AddonData>();
  if (data == nullptr || !data->buffer_release_tsfn) {
    ThrowShape(env, "internal: pinned-buffer TSFN unavailable");
  }

  auto* pin = new PinnedBuffer{env, nullptr};
  napi_status st = napi_create_reference(env, source, /*ref_count*/ 1, &pin->ref);
  if (st != napi_ok) {
    delete pin;
    ThrowShape(env, "internal: failed to pin source buffer");
  }

  Napi::ThreadSafeFunction tsfn = data->buffer_release_tsfn;
  napi_status acq = tsfn.Acquire();
  if (acq != napi_ok) {
    napi_delete_reference(env, pin->ref);
    delete pin;
    ThrowShape(env, "internal: failed to acquire pinned-buffer TSFN");
  }

  return [pin, tsfn](const T*) mutable {
    tsfn.BlockingCall(pin, ReleasePinnedOnJs);
    tsfn.Release();
  };
}

// No-op deleter used for zero-length buffers — nothing to pin, nothing to
// release. The native side receives (nullptr, 0); the C wrapper tolerates it.
template <typename T>
std::function<void(const T*)> NoopDeleter() {
  return [](const T*) {};
}

// ── Item → JS object ────────────────────────────────────────────────────────

Napi::Object TextToJs(Napi::Env env, const foundry_local::Item& item) {
  auto content = item.GetText();
  Napi::Object out = Napi::Object::New(env);
  out.Set("type", Napi::String::New(env, "text"));
  out.Set("text", Napi::String::New(env, content.text));
  out.Set("textType", Napi::String::New(env, TextTypeToString(content.type)));
  return out;
}

Napi::Object MessageToJs(Napi::Env env, const foundry_local::Item& item) {
  auto content = item.GetMessage();
  Napi::Object out = Napi::Object::New(env);
  out.Set("type", Napi::String::New(env, "message"));
  out.Set("role", Napi::String::New(env, RoleToString(content.role)));
  if (content.name.has_value()) {
    out.Set("name", Napi::String::New(env, std::string(*content.name)));
  }
  Napi::Array parts = Napi::Array::New(env, content.parts.size());
  for (size_t i = 0; i < content.parts.size(); ++i) {
    parts.Set(static_cast<uint32_t>(i), ItemToJs(env, content.parts[i]));
  }
  out.Set("parts", parts);
  if (content.IsSimpleText()) {
    out.Set("content", Napi::String::New(env, content.GetSimpleText()));
  }
  return out;
}

Napi::Object BytesToJs(Napi::Env env, const foundry_local::Item& item) {
  auto content = item.GetBytes();
  Napi::Object out = Napi::Object::New(env);
  out.Set("type", Napi::String::New(env, "bytes"));
  out.Set("itemType", Napi::String::New(env, ItemTypeToString(content.item_type)));
  out.Set("data", BufferCopy(env, content.data, content.data_size));
  return out;
}

Napi::Object TensorToJs(Napi::Env env, const foundry_local::Item& item) {
  auto content = item.GetTensor();
  Napi::Object out = Napi::Object::New(env);
  out.Set("type", Napi::String::New(env, "tensor"));
  out.Set("dataType", Napi::String::New(env, TensorDataTypeToString(content.data_type)));
  // Best-effort byte count: when dataType is opaque (STRING etc.) the byte
  // count is unknowable from shape alone; in that case we expose an empty
  // buffer rather than misreporting. Numeric types are the common case for JS.
  size_t elem_size = 0;
  switch (content.data_type) {
    case FOUNDRY_LOCAL_TENSOR_FLOAT:
      elem_size = 4;
      break;
    case FOUNDRY_LOCAL_TENSOR_UINT8:
    case FOUNDRY_LOCAL_TENSOR_INT8:
    case FOUNDRY_LOCAL_TENSOR_BOOL:
      elem_size = 1;
      break;
    case FOUNDRY_LOCAL_TENSOR_UINT16:
    case FOUNDRY_LOCAL_TENSOR_INT16:
    case FOUNDRY_LOCAL_TENSOR_FLOAT16:
      elem_size = 2;
      break;
    case FOUNDRY_LOCAL_TENSOR_INT32:
    case FOUNDRY_LOCAL_TENSOR_UINT32:
      elem_size = 4;
      break;
    case FOUNDRY_LOCAL_TENSOR_INT64:
    case FOUNDRY_LOCAL_TENSOR_UINT64:
    case FOUNDRY_LOCAL_TENSOR_DOUBLE:
      elem_size = 8;
      break;
    default:
      elem_size = 0;
      break;
  }
  size_t elem_count = 1;
  for (auto d : content.shape) {
    if (d <= 0) {
      elem_count = 0;
      break;
    }
    elem_count *= static_cast<size_t>(d);
  }
  size_t byte_count = elem_size * elem_count;
  out.Set("data", BufferCopy(env, content.data, byte_count));
  Napi::Array shape = Napi::Array::New(env, content.shape.size());
  for (size_t i = 0; i < content.shape.size(); ++i) {
    shape.Set(static_cast<uint32_t>(i), Napi::Number::New(env, static_cast<double>(content.shape[i])));
  }
  out.Set("shape", shape);
  return out;
}

Napi::Object ImageToJs(Napi::Env env, const foundry_local::Item& item) {
  auto content = item.GetImage();
  Napi::Object out = Napi::Object::New(env);
  out.Set("type", Napi::String::New(env, "image"));
  if (content.uri.has_value()) {
    out.Set("uri", Napi::String::New(env, std::string(*content.uri)));
  }
  if (content.format.has_value()) {
    out.Set("format", Napi::String::New(env, std::string(*content.format)));
  }
  if (content.data != nullptr && content.data_size > 0) {
    out.Set("data", BufferCopy(env, content.data, content.data_size));
  }
  return out;
}

Napi::Object AudioToJs(Napi::Env env, const foundry_local::Item& item) {
  auto content = item.GetAudio();
  Napi::Object out = Napi::Object::New(env);
  out.Set("type", Napi::String::New(env, "audio"));
  if (content.uri.has_value()) {
    out.Set("uri", Napi::String::New(env, std::string(*content.uri)));
  }
  if (content.format.has_value()) {
    out.Set("format", Napi::String::New(env, std::string(*content.format)));
  }
  if (content.data != nullptr && content.data_size > 0) {
    out.Set("data", BufferCopy(env, content.data, content.data_size));
  }
  if (content.sample_rate > 0) {
    out.Set("sampleRate", Napi::Number::New(env, content.sample_rate));
  }
  if (content.channels > 0) {
    out.Set("channels", Napi::Number::New(env, content.channels));
  }
  return out;
}

Napi::Object ToolCallToJs(Napi::Env env, const foundry_local::Item& item) {
  auto content = item.GetToolCall();
  Napi::Object out = Napi::Object::New(env);
  out.Set("type", Napi::String::New(env, "toolCall"));
  out.Set("callId", Napi::String::New(env, content.call_id));
  out.Set("name", Napi::String::New(env, content.name));
  out.Set("arguments", Napi::String::New(env, content.arguments));
  return out;
}

Napi::Object ToolResultToJs(Napi::Env env, const foundry_local::Item& item) {
  auto content = item.GetToolResult();
  Napi::Object out = Napi::Object::New(env);
  out.Set("type", Napi::String::New(env, "toolResult"));
  out.Set("callId", Napi::String::New(env, content.call_id));
  out.Set("result", Napi::String::New(env, content.result));
  return out;
}

[[noreturn]] void ThrowShape(Napi::Env env, const std::string& msg);

// ── JS object → Item ────────────────────────────────────────────────────────

[[noreturn]] void ThrowShape(Napi::Env env, const std::string& msg) {
  throw Napi::TypeError::New(env, msg);
}

std::optional<std::string> OptString(const Napi::Object& obj, const char* key) {
  if (!obj.Has(key)) return std::nullopt;
  Napi::Value v = obj.Get(key);
  if (v.IsUndefined() || v.IsNull()) return std::nullopt;
  if (!v.IsString()) return std::nullopt;
  return std::optional<std::string>(v.As<Napi::String>().Utf8Value());
}

std::string ReqString(Napi::Env env, const Napi::Object& obj, const char* key, const char* item_type_tag) {
  if (!obj.Has(key)) {
    ThrowShape(env, std::string("Item[type=") + item_type_tag + "] missing required field '" + key + "'");
  }
  Napi::Value v = obj.Get(key);
  if (!v.IsString()) {
    ThrowShape(env, std::string("Item[type=") + item_type_tag + "].'" + key + "' must be a string");
  }
  return v.As<Napi::String>().Utf8Value();
}

foundry_local::Item JsToTextItem(Napi::Env env, const Napi::Object& obj) {
  std::string text = ReqString(env, obj, "text", "text");
  flTextItemType type = FOUNDRY_LOCAL_TEXT_ITEM_TYPE_DEFAULT;
  if (auto t = OptString(obj, "textType"); t.has_value()) {
    if (!TextTypeFromString(*t, type)) {
      ThrowShape(env, "Item[type=text].textType: unknown value '" + *t + "'");
    }
  }
  return foundry_local::Item::Text(text, type);
}

foundry_local::Item JsToMessageItem(Napi::Env env, const Napi::Object& obj) {
  std::string role_s = ReqString(env, obj, "role", "message");
  flMessageRole role;
  if (!RoleFromString(role_s, role)) {
    ThrowShape(env, "Item[type=message].role: unknown value '" + role_s + "'");
  }
  auto name = OptString(obj, "name");
  std::optional<std::string> name_opt = name.has_value() ? std::optional<std::string>(*name) : std::nullopt;

  // 'parts' (array) takes precedence over 'content' (single string).
  if (obj.Has("parts") && !obj.Get("parts").IsUndefined() && !obj.Get("parts").IsNull()) {
    Napi::Value parts_v = obj.Get("parts");
    if (!parts_v.IsArray()) {
      ThrowShape(env, "Item[type=message].parts must be an array of items");
    }
    Napi::Array parts = parts_v.As<Napi::Array>();
    std::vector<foundry_local::Item> built;
    built.reserve(parts.Length());
    for (uint32_t i = 0; i < parts.Length(); ++i) {
      built.emplace_back(JsToItem(env, parts.Get(i)));
    }
    return foundry_local::MessageItem(role, std::move(built), name_opt);
  }

  // Single-text convenience.
  std::string content = ReqString(env, obj, "content", "message");
  return foundry_local::MessageItem(role, content, name_opt);
}

foundry_local::Item JsToToolCallItem(Napi::Env env, const Napi::Object& obj) {
  std::string call_id = ReqString(env, obj, "callId", "toolCall");
  std::string name = ReqString(env, obj, "name", "toolCall");
  std::string arguments = ReqString(env, obj, "arguments", "toolCall");
  return foundry_local::Item::ToolCall(call_id, name, arguments);
}

foundry_local::Item JsToToolResultItem(Napi::Env env, const Napi::Object& obj) {
  std::string call_id = ReqString(env, obj, "callId", "toolResult");
  std::string result = ReqString(env, obj, "result", "toolResult");
  return foundry_local::Item::ToolResult(call_id, result);
}

foundry_local::Item JsToBytesItem(Napi::Env env, const Napi::Object& obj) {
  auto src_opt = OptBytesValue(obj, "data");
  if (!src_opt.has_value()) {
    ThrowShape(env, "Item[type=bytes]: 'data' (Uint8Array|Buffer|ArrayBuffer) is required");
  }
  Napi::Value src = *src_opt;
  BytesView view = ParseBytesValue(env, src, "bytes", "data");

  // Zero-length: skip the pin entirely; no reference to create, no Release
  // to perform. The C wrapper accepts (nullptr, 0).
  if (view.size == 0) {
    return foundry_local::Item::Bytes(
        FOUNDRY_LOCAL_ITEM_BYTES, nullptr, 0, NoopDeleter<flBytesData>());
  }

  return foundry_local::Item::Bytes(
      FOUNDRY_LOCAL_ITEM_BYTES,
      const_cast<void*>(static_cast<const void*>(view.data)),
      view.size,
      MakePinnedDeleter<flBytesData>(env, src));
}

foundry_local::Item JsToTensorItem(Napi::Env env, const Napi::Object& obj) {
  std::string dt_s = ReqString(env, obj, "dataType", "tensor");
  flTensorDataType dt;
  if (!TensorDataTypeFromString(dt_s, dt)) {
    ThrowShape(env, "Item[type=tensor].dataType: unsupported value '" + dt_s + "'");
  }
  size_t elem_size = TensorElemSize(dt);
  if (elem_size == 0) {
    ThrowShape(env, "Item[type=tensor].dataType: '" + dt_s +
                        "' is not supported as a Request input (no portable element size)");
  }

  if (!obj.Has("shape") || !obj.Get("shape").IsArray()) {
    ThrowShape(env, "Item[type=tensor].shape must be an array of integers");
  }
  Napi::Array shape_arr = obj.Get("shape").As<Napi::Array>();
  std::vector<int64_t> shape;
  shape.reserve(shape_arr.Length());
  size_t elem_count = 1;
  for (uint32_t i = 0; i < shape_arr.Length(); ++i) {
    Napi::Value d = shape_arr.Get(i);
    if (!d.IsNumber()) {
      ThrowShape(env, "Item[type=tensor].shape entries must be numbers");
    }
    int64_t dim = d.As<Napi::Number>().Int64Value();
    if (dim <= 0) {
      ThrowShape(env, "Item[type=tensor].shape entries must be positive");
    }
    shape.push_back(dim);
    elem_count *= static_cast<size_t>(dim);
  }

  // Fail fast on shape/size mismatch BEFORE creating the pinning reference
  // so an invalid input never leaks an N-API ref or a TSFN Acquire.
  auto src_opt = OptBytesValue(obj, "data");
  if (!src_opt.has_value()) {
    ThrowShape(env, "Item[type=tensor]: 'data' (Uint8Array|Buffer|ArrayBuffer) is required");
  }
  Napi::Value src = *src_opt;
  BytesView view = ParseBytesValue(env, src, "tensor", "data");
  size_t expected = elem_size * elem_count;
  if (view.size != expected) {
    ThrowShape(env, "Item[type=tensor].data: expected " + std::to_string(expected) +
                        " bytes (" + dt_s + " * shape), got " + std::to_string(view.size));
  }

  if (view.size == 0) {
    return foundry_local::Item::Tensor(
        dt, nullptr, shape.data(), shape.size(), NoopDeleter<flTensorData>());
  }

  return foundry_local::Item::Tensor(
      dt,
      const_cast<void*>(static_cast<const void*>(view.data)),
      shape.data(), shape.size(),
      MakePinnedDeleter<flTensorData>(env, src));
}

foundry_local::Item JsToImageItem(Napi::Env env, const Napi::Object& obj) {
  auto uri = OptString(obj, "uri");
  auto src_opt = OptBytesValue(obj, "data");
  bool has_uri = uri.has_value();
  bool has_data = src_opt.has_value();
  if (has_uri == has_data) {
    ThrowShape(env, "Item[type=image]: exactly one of 'uri' or 'data' is required");
  }

  auto format = OptString(obj, "format");
  if (has_uri) {
    std::optional<std::string> format_opt =
        format.has_value() ? std::optional<std::string>(*format) : std::nullopt;
    return foundry_local::Item::ImageFromUri(*uri, format_opt);
  }

  if (!format.has_value()) {
    ThrowShape(env, "Item[type=image]: 'format' is required when constructing from 'data'");
  }

  Napi::Value src = *src_opt;
  BytesView view = ParseBytesValue(env, src, "image", "data");

  if (view.size == 0) {
    return foundry_local::Item::ImageFromData(
        *format, nullptr, 0, NoopDeleter<flImageData>());
  }

  return foundry_local::Item::ImageFromData(
      *format,
      const_cast<void*>(static_cast<const void*>(view.data)),
      view.size,
      MakePinnedDeleter<flImageData>(env, src));
}

foundry_local::Item JsToAudioItem(Napi::Env env, const Napi::Object& obj) {
  auto uri = OptString(obj, "uri");
  auto src_opt = OptBytesValue(obj, "data");
  bool has_uri = uri.has_value();
  bool has_data = src_opt.has_value();
  if (has_uri && has_data) {
    ThrowShape(env, "Item[type=audio]: 'uri' and 'data' are mutually exclusive");
  }

  auto format = OptString(obj, "format");
  bool has_sample_rate = obj.Has("sampleRate") && obj.Get("sampleRate").IsNumber();
  bool has_channels = obj.Has("channels") && obj.Get("channels").IsNumber();
  int sample_rate = has_sample_rate ? obj.Get("sampleRate").As<Napi::Number>().Int32Value() : 0;
  int channels = has_channels ? obj.Get("channels").As<Napi::Number>().Int32Value() : 0;

  // Streaming-descriptor case: neither uri nor data. Bytes will be supplied
  // by an accompanying ItemQueue. Require the full format triple so the
  // model knows how to interpret the streamed bytes; this also prevents an
  // accidental empty audio item slipping through validation.
  if (!has_uri && !has_data) {
    if (!format.has_value() || !has_sample_rate || !has_channels) {
      ThrowShape(env,
                 "Item[type=audio]: must have either 'uri', 'data', or "
                 "(format + sampleRate + channels) for a streaming descriptor");
    }
    // Non-owning AudioFromData overload — no deleter, so the C ABI's
    // "deleter requires mutable_data" check is not triggered for the
    // (data == nullptr, data_size == 0) descriptor shape.
    return foundry_local::Item::AudioFromData(
        *format, static_cast<const void*>(nullptr), 0, sample_rate, channels);
  }

  if (has_uri) {
    std::optional<std::string> format_opt =
        format.has_value() ? std::optional<std::string>(*format) : std::nullopt;
    return foundry_local::Item::AudioFromUri(*uri, format_opt, sample_rate, channels);
  }

  if (!format.has_value()) {
    ThrowShape(env, "Item[type=audio]: 'format' is required when constructing from 'data'");
  }

  Napi::Value src = *src_opt;
  BytesView view = ParseBytesValue(env, src, "audio", "data");

  if (view.size == 0) {
    return foundry_local::Item::AudioFromData(
        *format, nullptr, 0, NoopDeleter<flAudioData>(), sample_rate, channels);
  }

  return foundry_local::Item::AudioFromData(
      *format,
      const_cast<void*>(static_cast<const void*>(view.data)),
      view.size,
      MakePinnedDeleter<flAudioData>(env, src),
      sample_rate, channels);
}

}  // namespace

Napi::Value ItemToJs(Napi::Env env, const foundry_local::Item& item) {
  flItemType t = item.GetType();
  switch (t) {
    case FOUNDRY_LOCAL_ITEM_TEXT:
      return TextToJs(env, item);
    case FOUNDRY_LOCAL_ITEM_MESSAGE:
      return MessageToJs(env, item);
    case FOUNDRY_LOCAL_ITEM_BYTES:
      return BytesToJs(env, item);
    case FOUNDRY_LOCAL_ITEM_TENSOR:
      return TensorToJs(env, item);
    case FOUNDRY_LOCAL_ITEM_IMAGE:
      return ImageToJs(env, item);
    case FOUNDRY_LOCAL_ITEM_AUDIO:
      return AudioToJs(env, item);
    case FOUNDRY_LOCAL_ITEM_TOOL_CALL:
      return ToolCallToJs(env, item);
    case FOUNDRY_LOCAL_ITEM_TOOL_RESULT:
      return ToolResultToJs(env, item);
    default: {
      Napi::Object out = Napi::Object::New(env);
      out.Set("type", Napi::String::New(env, ItemTypeToString(t)));
      return out;
    }
  }
}

foundry_local::Item JsToItem(Napi::Env env, const Napi::Value& value) {
  if (!value.IsObject() || value.IsArray()) {
    ThrowShape(env, "Item: expected a plain object with a 'type' discriminator");
  }
  Napi::Object obj = value.As<Napi::Object>();
  std::string type = ReqString(env, obj, "type", "?");

  if (type == "text") return JsToTextItem(env, obj);
  if (type == "message") return JsToMessageItem(env, obj);
  if (type == "toolCall") return JsToToolCallItem(env, obj);
  if (type == "toolResult") return JsToToolResultItem(env, obj);
  if (type == "image") return JsToImageItem(env, obj);
  if (type == "audio") return JsToAudioItem(env, obj);
  if (type == "bytes") return JsToBytesItem(env, obj);
  if (type == "tensor") return JsToTensorItem(env, obj);
  ThrowShape(env, "Item: unknown type discriminator '" + type + "'");
}

}  // namespace foundry_local_node
