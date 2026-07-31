// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "ep_detection/winml_provider_allowlist.h"

#include <gtest/gtest.h>

#include <string_view>

using namespace fl;

TEST(WinMLProviderAllowlistTest, AcceptsKnownProvidersCaseInsensitively) {
  constexpr std::string_view trusted_providers[] = {
      "MIGraphXExecutionProvider",
      "NvTensorRtRtxExecutionProvider",
      "NvTensorRTRTXExecutionProvider",
      "OpenVINOExecutionProvider",
      "QNNExecutionProvider",
      "qnnexecutionprovider",
      "RyzenAILightExecutionProvider",
      "VitisAIExecutionProvider",
  };

  for (const auto provider : trusted_providers) {
    EXPECT_TRUE(IsTrustedWinMLProvider(provider)) << provider;
  }
}

TEST(WinMLProviderAllowlistTest, RejectsUnapprovedProviders) {
  constexpr std::string_view unapproved_providers[] = {
      "",
      "WebGPUExecutionProvider",
      "CPUExecutionProvider",
      "DmlExecutionProvider",
      "FutureExecutionProvider",
  };

  for (const auto provider : unapproved_providers) {
    EXPECT_FALSE(IsTrustedWinMLProvider(provider)) << provider;
  }
}
