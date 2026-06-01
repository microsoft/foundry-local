// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// Helper for converting a JS `RequestOptions` object into the typed
// `foundry_local::RequestOptions` struct. Shared between `Request::SetOptions`
// and the various `*Session::SetOptions` entry points.
//
// Expected JS shape (mirrors the C++ struct):
//   {
//     search?: {
//       temperature?: number,
//       topP?: number,
//       topK?: number,
//       maxOutputTokens?: number,
//       frequencyPenalty?: number,
//       presencePenalty?: number,
//       seed?: number,
//       earlyStopping?: boolean,
//       doSample?: boolean,
//     },
//     toolChoice?: "auto" | "none" | "required",
//     additionalOptions?: { [key: string]: string | number | boolean | undefined },
//   }
#pragma once

#include <napi.h>

#include <foundry_local/foundry_local_cpp.h>

namespace foundry_local_node {

// Build a foundry_local::RequestOptions from a JS object. Throws
// Napi::TypeError on malformed input — callers wrap in CallChecked().
foundry_local::RequestOptions JsToRequestOptions(Napi::Env env, const Napi::Object& opts);

}  // namespace foundry_local_node
