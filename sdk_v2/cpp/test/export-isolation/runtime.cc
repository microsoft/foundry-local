// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <foundry_local/foundry_local_c.h>

#include <cstring>

int main() {
  return std::strcmp(FoundryLocalGetVersionString(), "42") == 0 ? 0 : 1;
}
