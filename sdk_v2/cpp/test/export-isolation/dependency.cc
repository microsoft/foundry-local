// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

extern "C" int FoundryLocalArchiveImplementation() {
  return 20;
}

extern "C" __attribute__((weak)) int FoundryLocalWeakArchiveImplementation() {
  return 22;
}
