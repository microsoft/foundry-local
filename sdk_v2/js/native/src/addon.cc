// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Node-API module entry. Module init runs once per process, on the JS thread,
// before any addon export is called.
#include "addon_data.h"
#include "catalog.h"
#include "item_queue.h"
#include "manager.h"
#include "model.h"
#include "request.h"
#include "session.h"

#include <napi.h>

#if defined(__linux__) && defined(__GLIBC__)
// Required so <dlfcn.h> exposes RTLD_DEEPBIND (a glibc extension).
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <dlfcn.h>
#endif

namespace foundry_local_node {

// ── Preload system OpenSSL with RTLD_DEEPBIND on Linux/glibc ────────────
//
// Carried over verbatim from sdk/js/native/foundry_local_napi.c.
// See that file (and ort-loading-contract.instructions.md) for the full
// rationale. Summary: Node exports its statically-linked OpenSSL symbols
// globally; when foundry_local's transitive deps later pull in the system
// libcrypto/libssl, libcrypto's own internal calls bind to Node's
// incompatible OpenSSL build and crash on the first HTTPS path. Force
// libcrypto -> libssl to load themselves first with RTLD_DEEPBIND so their
// internal references stay inside their own scope.
//
// This is a no-op on macOS (dyld two-level namespace already isolates) and
// Windows (LoadLibrary is unaffected by this class of clobber).
static void PreloadIsolatedOpenSsl() {
#if defined(__linux__) && defined(__GLIBC__) && defined(RTLD_DEEPBIND)
  static void* s_libcrypto = nullptr;
  static void* s_libssl = nullptr;
  if (s_libcrypto != nullptr) {
    return;
  }
  const int flags = RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND;
  static const char* const crypto_sonames[] = {"libcrypto.so.3", "libcrypto.so.1.1", nullptr};
  static const char* const ssl_sonames[] = {"libssl.so.3", "libssl.so.1.1", nullptr};
  for (size_t i = 0; crypto_sonames[i] != nullptr && s_libcrypto == nullptr; ++i) {
    s_libcrypto = dlopen(crypto_sonames[i], flags);
  }
  if (s_libcrypto == nullptr) {
    return;
  }
  for (size_t i = 0; ssl_sonames[i] != nullptr && s_libssl == nullptr; ++i) {
    s_libssl = dlopen(ssl_sonames[i], flags);
  }
#endif
}

}  // namespace foundry_local_node

namespace foundry_local_node {

AddonData::~AddonData() {
  // buffer_release_tsfn is released by the env cleanup hook registered in
  // Init(), not by this destructor. Releasing a ThreadSafeFunction from the
  // instance-data destructor is unsafe: it runs late in node::FreeEnvironment,
  // after the point where the TSFN's uv_async handle can be finalized cleanly,
  // so the async close races the loop teardown and Node dereferences freed
  // state during uv_run (a process-exit access violation). N-API's
  // "Finalization on the exit of the environment" guidance prescribes
  // napi_add_env_cleanup_hook for exactly this. The cleanup hook nulls the
  // handle, so this guard is normally false; the Release() below is a
  // defensive fallback that only runs if the cleanup hook never fired.
  if (buffer_release_tsfn) {
    buffer_release_tsfn.Release();
  }
}

}  // namespace foundry_local_node

static Napi::Object Init(Napi::Env env, Napi::Object exports) {
  foundry_local_node::PreloadIsolatedOpenSsl();

  // Per-Env addon data holds FunctionReferences to ObjectWrap classes that
  // the addon needs to instantiate from C++ (Catalog and Model are built
  // internally — never via direct `new` from JS).
  auto* data = new foundry_local_node::AddonData();
  env.SetInstanceData<foundry_local_node::AddonData>(data);

  // Shared TSFN for pinned-buffer deleters — see comment on
  // AddonData::buffer_release_tsfn. The JS callback is a no-op; the real
  // work runs in the per-call data callback passed to BlockingCall().
  Napi::Function noop_release = Napi::Function::New(env, [](const Napi::CallbackInfo&) {});
  data->buffer_release_tsfn = Napi::ThreadSafeFunction::New(
      env, noop_release, "foundry_local_node.buffer_release", /*maxQueueSize*/ 0,
      /*initialThreadCount*/ 1);
  // Unref the TSFN so it does NOT pin the libuv event loop. Without this,
  // Node will never exit on its own — the addon-held TSFN reference keeps
  // the loop alive until ~AddonData (env teardown), which itself can only
  // run once the loop drains: a deadlock that hangs every sample that
  // doesn't explicitly call process.exit(). Pending deleter callbacks
  // queued from background threads still execute on the JS thread; we just
  // stop using this TSFN to artificially keep the process alive.
  data->buffer_release_tsfn.Unref(env);

  // Release the TSFN from an env cleanup hook rather than from ~AddonData.
  // Cleanup hooks run early in node::FreeEnvironment, while the event loop
  // can still finalize the TSFN's uv_async handle safely; the instance-data
  // destructor runs too late and its async close races loop teardown,
  // producing a process-exit access violation (a use-after-free that Node
  // hits while draining the loop in uv_run). Null the handle so ~AddonData
  // does not double-release. Per N-API "Finalization on the exit of the
  // Node.js environment".
  env.AddCleanupHook(
      [](foundry_local_node::AddonData* d) {
        if (d->buffer_release_tsfn) {
          d->buffer_release_tsfn.Release();
          d->buffer_release_tsfn = Napi::ThreadSafeFunction();
        }
      },
      data);

  Napi::Function manager_ctor = foundry_local_node::Manager::Init(env);
  Napi::Function catalog_ctor = foundry_local_node::Catalog::Init(env);
  Napi::Function model_ctor = foundry_local_node::Model::Init(env);
  Napi::Function request_ctor = foundry_local_node::Request::Init(env);
  Napi::Function item_queue_ctor = foundry_local_node::NativeItemQueue::Init(env);
  Napi::Function chat_session_ctor = foundry_local_node::ChatSession::Init(env);
  Napi::Function embeddings_session_ctor = foundry_local_node::EmbeddingsSession::Init(env);
  Napi::Function audio_session_ctor = foundry_local_node::AudioSession::Init(env);

  data->catalog_ctor = Napi::Persistent(catalog_ctor);
  data->model_ctor = Napi::Persistent(model_ctor);
  data->request_ctor = Napi::Persistent(request_ctor);
  data->item_queue_ctor = Napi::Persistent(item_queue_ctor);

  exports.Set("Manager", manager_ctor);
  // Catalog and Model constructors are exported for `instanceof` checks on the
  // JS side, but direct construction (`new addon.Catalog()`) throws TypeError.
  exports.Set("Catalog", catalog_ctor);
  exports.Set("Model", model_ctor);
  // Request is directly constructible from JS (it's a stateful builder).
  exports.Set("Request", request_ctor);
  // ItemQueue is directly constructible from JS — it's a stateful native
  // handle exposed by the public `ItemQueue` TS class.
  exports.Set("ItemQueue", item_queue_ctor);
  // Only modality-specific session classes are constructible from JS:
  // `new ChatSession(model)`, `new EmbeddingsSession(model)`,
  // `new AudioSession(model)`. The abstract TS base `Session` has no
  // native counterpart.
  exports.Set("ChatSession", chat_session_ctor);
  exports.Set("EmbeddingsSession", embeddings_session_ctor);
  exports.Set("AudioSession", audio_session_ctor);
  return exports;
}

NODE_API_MODULE(foundry_local_node, Init)
