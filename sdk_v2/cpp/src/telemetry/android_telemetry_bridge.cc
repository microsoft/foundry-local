// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "http/HttpClient_Android.hpp"

extern "C" __attribute__((visibility("default"))) bool FoundryLocalIsAndroidTelemetryReady() noexcept {
  try {
    return Microsoft::Applications::Events::HttpClient_Android::GetClientInstance() != nullptr;
  } catch (...) {
    return false;
  }
}
