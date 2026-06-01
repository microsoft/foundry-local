// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// foundry_local::Error -> Napi::Error translator.
// Every JS-facing native entry runs its wrapper calls inside CallChecked() so
// that wrapper exceptions become typed JS errors and any other std::exception
// becomes a generic Napi::Error. The wrapper has already translated flStatus*
// into foundry_local::Error — addon code never needs to inspect flStatus.
#pragma once

#include <napi.h>

#include <functional>

namespace foundry_local_node {

// Run `fn` and translate any thrown exception into a Napi error pending on
// `env`, then return a default-constructed T. Callers should test for a
// pending exception after this returns and bail early.
template <typename T>
T CallChecked(Napi::Env env, const std::function<T()>& fn);

// Specialization for void.
void CallCheckedVoid(Napi::Env env, const std::function<void()>& fn);

}  // namespace foundry_local_node
