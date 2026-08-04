// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Conversion helpers between JS-side discriminated-union Item objects and
// `foundry_local::Item` from the C++ wrapper.
//
// The JS surface intentionally uses plain JS objects rather than ObjectWrap
// classes for items:
//   * inputs (Request.addItem) — caller passes `{ type: 'message', ... }`
//   * outputs (Response output items) — addon returns the same shape
// This keeps the API ergonomic. Binary outputs can retain an owning native
// object through an external ArrayBuffer, avoiding a data copy.
//
// Supported subtypes (both directions): text, message (with optional typed
// parts), bytes, tensor, image (uri or in-memory data), audio (uri,
// in-memory data, or descriptor-only for streaming via ItemQueue), toolCall,
// toolResult.
//
// In-memory image/audio/bytes/tensor inputs are pinned for the owning
// Request's lifetime — the addon does not copy the underlying buffer. See
// items.cc for the per-subtype pinning details.
#pragma once

#include <napi.h>

#include <foundry_local/foundry_local_cpp.h>

#include <memory>

namespace foundry_local_node {

/// Convert a C++ Item to a JS plain object. When owner is provided, binary data
/// directly views native memory and retains owner until the JS view is collected.
/// Without an owner, binary data is copied so borrowed Items remain safe.
Napi::Value ItemToJs(Napi::Env env, const foundry_local::Item& item,
                     std::shared_ptr<void> owner = nullptr);

/// Convert a JS plain object describing an input item into an owning
/// `foundry_local::Item`. Throws `Napi::TypeError` for an invalid shape.
/// The returned Item is move-only and owns its underlying flItem.
foundry_local::Item JsToItem(Napi::Env env, const Napi::Value& value);

}  // namespace foundry_local_node
