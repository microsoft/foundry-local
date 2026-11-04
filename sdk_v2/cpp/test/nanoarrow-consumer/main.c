// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include <nanoarrow/nanoarrow_ipc.h>

int main(void) {
  struct ArrowIpcDecoder decoder;
  int result = ArrowIpcDecoderInit(&decoder);
  if (result != 0) return result;
  ArrowIpcDecoderReset(&decoder);
  return 0;
}
