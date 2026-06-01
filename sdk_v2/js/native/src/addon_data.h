// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Per-Env addon data. Holds Napi::FunctionReference constructors for the
// ObjectWrap classes that the addon needs to instantiate from C++ (Catalog
// and Model are manufactured by Manager methods and Catalog methods — never
// by direct JS construction). Lifetime is tied to the Napi::Env.
#pragma once

#include <napi.h>

namespace foundry_local_node {

struct AddonData {
  Napi::FunctionReference catalog_ctor;
  Napi::FunctionReference model_ctor;
  Napi::FunctionReference request_ctor;
  // Persistent reference to the NativeItemQueue constructor so that
  // Request::AddItem can branch on `InstanceOf` to decide between the
  // plain-object Item path (which transfers ownership) and the
  // queue-borrow path (which calls AddItem(item, take_ownership=false)).
  Napi::FunctionReference item_queue_ctor;

  // Shared TSFN used by the pinned-buffer deleter for raw-bytes Item inputs
  // (bytes / tensor / image-from-data / audio-from-data). The deleter must
  // call napi_delete_reference on the JS thread; today the deleter fires on
  // the JS thread (Request-owned items finalized via ObjectWrap), but once
  // ItemQueue lands the deleter may fire on a native consumer thread —
  // bouncing through this TSFN keeps both paths uniform and safe.
  //
  // Created once at addon Init with an initial thread count of 1 (the
  // addon's own reference). Each MakePinnedDeleter call Acquires; each
  // deleter invocation Releases. The addon's reference is released by
  // ~AddonData at env teardown so the TSFN finalizes after all pending
  // pinned-buffer deleters have drained.
  Napi::ThreadSafeFunction buffer_release_tsfn;

  ~AddonData();
};

}  // namespace foundry_local_node
