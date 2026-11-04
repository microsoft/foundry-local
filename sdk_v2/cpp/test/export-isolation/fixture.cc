// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <foundry_local/foundry_local_c.h>

extern "C" int FoundryLocalArchiveImplementation();
extern "C" int FoundryLocalWeakArchiveImplementation();

const flApi* FL_API_CALL FoundryLocalGetApi(uint32_t) noexcept {
  return nullptr;
}

const char* FL_API_CALL FoundryLocalGetVersionString() noexcept {
  return FoundryLocalArchiveImplementation() + FoundryLocalWeakArchiveImplementation() == 42 ? "42" : "unexpected";
}
