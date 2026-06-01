// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Standalone Node-API addon whose only export is `preloadLibrary(absolutePath: string): void`. It exists
// because Node 23+ rejects `process.dlopen` for shared libraries that are not Node-API addons ("Module did
// not self-register"), and our `foundry_local.{dll,so,dylib}` and its ORT/GenAI siblings are plain C++
// libraries. This addon is link-independent from foundry_local (and everything else) so it can be loaded
// safely before foundry_local's deps are resident in the process.
//
// On Windows the implementation calls `LoadLibraryExW(..., LOAD_WITH_ALTERED_SEARCH_PATH)`; on POSIX it
// calls `dlopen(path, RTLD_NOW | RTLD_GLOBAL)`. Failures throw a JS `Error` whose message includes the path
// and the platform error string. The loaded handle is intentionally never released — preloaded libraries
// stay resident for the process lifetime, matching the C# and Python bindings.
#include <napi.h>

#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

#if defined(_WIN32)
static std::string FormatLastError(DWORD code) {
  LPSTR buffer = nullptr;
  const DWORD len = ::FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
  std::string message;
  if (len > 0 && buffer != nullptr) {
    message.assign(buffer, len);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ')) {
      message.pop_back();
    }
  }
  if (buffer != nullptr) {
    ::LocalFree(buffer);
  }
  if (message.empty()) {
    message = "unknown Win32 error";
  }
  return message + " (code " + std::to_string(code) + ")";
}

static std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) {
    return std::wstring();
  }
  const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
  return wide;
}
#endif

static Napi::Value PreloadLibrary(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsString()) {
    Napi::TypeError::New(env, "preloadLibrary(path: string) requires a string argument").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const std::string path = info[0].As<Napi::String>().Utf8Value();

#if defined(_WIN32)
  const std::wstring wpath = Utf8ToWide(path);
  if (wpath.empty() && !path.empty()) {
    Napi::Error::New(env, "Failed to convert path to UTF-16: " + path).ThrowAsJavaScriptException();
    return env.Undefined();
  }
  const HMODULE handle = ::LoadLibraryExW(wpath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (handle == nullptr) {
    const DWORD code = ::GetLastError();
    Napi::Error::New(env, "LoadLibraryExW failed for '" + path + "': " + FormatLastError(code))
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
#else
  void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (handle == nullptr) {
    const char* err = ::dlerror();
    const std::string detail = err != nullptr ? err : "unknown dlopen error";
    Napi::Error::New(env, "dlopen failed for '" + path + "': " + detail).ThrowAsJavaScriptException();
    return env.Undefined();
  }
#endif

  return env.Undefined();
}

}  // namespace

static Napi::Object Init(Napi::Env env, Napi::Object exports) {
  exports.Set("preloadLibrary", Napi::Function::New(env, PreloadLibrary, "preloadLibrary"));
  return exports;
}

NODE_API_MODULE(foundry_local_preload, Init)
