// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/execution_provider.h"

#include <gtest/gtest.h>

using namespace fl;

TEST(ExecutionProviderTest, StringToEPRecognizesSupportedNames) {
  EXPECT_EQ(EPUtils::StringtoEP("cpu"), ExecutionProvider::kCPU);
  EXPECT_EQ(EPUtils::StringtoEP("CPU"), ExecutionProvider::kCPU);
  EXPECT_EQ(EPUtils::StringtoEP("CPUExecutionProvider"), ExecutionProvider::kCPU);
  EXPECT_EQ(EPUtils::StringtoEP("cuda"), ExecutionProvider::kCUDA);
  EXPECT_EQ(EPUtils::StringtoEP("CUDAExecutionProvider"), ExecutionProvider::kCUDA);
  EXPECT_EQ(EPUtils::StringtoEP("WebGPU"), ExecutionProvider::kWebGPU);
  EXPECT_EQ(EPUtils::StringtoEP("OpenVINOExecutionProvider"), ExecutionProvider::kOpenVINO);
  EXPECT_EQ(EPUtils::StringtoEP("NvTensorRTRTXExecutionProvider"), ExecutionProvider::kTensorRT_RTX);
  EXPECT_EQ(EPUtils::StringtoEP("VitisAIExecutionProvider"), ExecutionProvider::kVitisAI);
  EXPECT_EQ(EPUtils::StringtoEP("RyzenAI"), ExecutionProvider::kRyzenAI);
  EXPECT_EQ(EPUtils::StringtoEP("QNNExecutionProvider"), ExecutionProvider::kQNN);
}

TEST(ExecutionProviderTest, StringToEPReturnsUnknownForUnsupportedOrEmptyNames) {
  EXPECT_EQ(EPUtils::StringtoEP(""), ExecutionProvider::kUnknown);
  EXPECT_EQ(EPUtils::StringtoEP("trt"), ExecutionProvider::kUnknown);
  EXPECT_EQ(EPUtils::StringtoEP("DirectMLExecutionProvider"), ExecutionProvider::kUnknown);
  EXPECT_EQ(EPUtils::StringtoEP("MIGraphXExecutionProvider"), ExecutionProvider::kUnknown);
  EXPECT_EQ(EPUtils::StringtoEP("migraphx"), ExecutionProvider::kUnknown);
}

TEST(ExecutionProviderTest, AmdGpuMappingsUseGenAiNameAndCanonicalRegistrationName) {
  for (const auto name : {"AMDGPU", "amdgpu", "AMDGPUExecutionProvider", "AmDgPuExEcUtIoNpRoViDeR"}) {
    EXPECT_EQ(EPUtils::StringtoEP(name), ExecutionProvider::kAMDGPU) << name;
  }
  EXPECT_EQ(static_cast<int>(ExecutionProvider::kQNN), 9);
  EXPECT_EQ(static_cast<int>(ExecutionProvider::kAMDGPU), 10);
  EXPECT_EQ(EPUtils::EPtoGenAI(ExecutionProvider::kAMDGPU), "AMDGPU");
  EXPECT_EQ(EPUtils::EPtoRegistrationName(ExecutionProvider::kAMDGPU), "AMDGPUExecutionProvider");
}

TEST(ExecutionProviderTest, EPtoGenAIReturnsExpectedProviderNames) {
  EXPECT_EQ(EPUtils::EPtoGenAI(ExecutionProvider::kDefault), "");
  EXPECT_EQ(EPUtils::EPtoGenAI(ExecutionProvider::kCPU), "");
  EXPECT_EQ(EPUtils::EPtoGenAI(ExecutionProvider::kCUDA), "cuda");
  EXPECT_EQ(EPUtils::EPtoGenAI(ExecutionProvider::kWebGPU), "WebGPU");
  EXPECT_EQ(EPUtils::EPtoGenAI(ExecutionProvider::kOpenVINO), "OpenVINO");
  EXPECT_EQ(EPUtils::EPtoGenAI(ExecutionProvider::kTensorRT_RTX), "NvTensorRtRtx");
  EXPECT_EQ(EPUtils::EPtoGenAI(ExecutionProvider::kVitisAI), "VitisAI");
  EXPECT_EQ(EPUtils::EPtoGenAI(ExecutionProvider::kRyzenAI), "RyzenAI");
  EXPECT_EQ(EPUtils::EPtoGenAI(ExecutionProvider::kQNN), "QNN");
}

TEST(ExecutionProviderTest, RoundTripPreservesSupportedNonDefaultProviders) {
  const auto providers = {
      ExecutionProvider::kCUDA,
      ExecutionProvider::kWebGPU,
      ExecutionProvider::kOpenVINO,
      ExecutionProvider::kTensorRT_RTX,
      ExecutionProvider::kVitisAI,
      ExecutionProvider::kRyzenAI,
      ExecutionProvider::kQNN,
      ExecutionProvider::kAMDGPU,
  };

  for (auto provider : providers) {
    EXPECT_EQ(EPUtils::StringtoEP(EPUtils::EPtoGenAI(provider)), provider);
  }
}
