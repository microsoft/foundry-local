// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <foundry_local/foundry_local_c.h>

#include <functional>

namespace fl {

struct StreamingCallback {
  using Function = std::function<int(flStreamingCallbackData, void*)>;
  Function function;
  void* user_data = nullptr;
};

}  // namespace fl
