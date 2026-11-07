// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#ifdef _WIN32
#include <windows.h>
#endif

namespace fl {

inline bool IsDllShutdownInProgress() noexcept {
#ifdef _WIN32
  using ShutdownInProgress = BOOLEAN(NTAPI*)();
  const auto ntdll = ::GetModuleHandleW(L"ntdll.dll");
  const auto shutdown = ntdll == nullptr
                            ? nullptr
                            : reinterpret_cast<ShutdownInProgress>(::GetProcAddress(ntdll, "RtlDllShutdownInProgress"));
  return shutdown != nullptr && shutdown();
#else
  return false;
#endif
}

}  // namespace fl
