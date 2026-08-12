// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <string>

namespace fl {

/// Enumeration of the execution providers we're aware of.
enum class ExecutionProvider {
  kUnknown = 0,  // Unable to convert
  kDefault = 1,  // Used to indicate the default provider for a model when the specific provider is not known.
  kCPU = 2,
  kCUDA = 3,
  kWebGPU = 4,
  kOpenVINO = 5,
  kTensorRT_RTX = 6,
  kVitisAI = 7,
  kRyzenAI = 8,
  kQNN = 9,
};

struct EPUtils {
  /// Convert a value from the catalog, genai config, or EP override param to an ExecutionProvider
  /// See NormalizeProviderName https://github.com/microsoft/onnxruntime-genai/blob/main/src/config.cpp
  /// See AppendExecutionProviderV1
  /// https://github.com/microsoft/onnxruntime-genai/blob/main/src/models/session_options.cpp
  static ExecutionProvider StringtoEP(std::string_view ep) {
    if (ep == "cpu" ||
        ep == "CPU" ||
        ep == "CPUExecutionProvider") {
      return ExecutionProvider::kCPU;
    } else if (ep == "cuda" ||
               ep == "CUDA" ||
               ep == "CUDAExecutionProvider") {
      return ExecutionProvider::kCUDA;
    } else if (ep == "webgpu" ||
               ep == "WebGPU" ||
               ep == "WebGPUExecutionProvider") {
      return ExecutionProvider::kWebGPU;
    } else if (ep == "openvino" ||
               ep == "OpenVINO" ||
               ep == "OpenVINOExecutionProvider") {
      return ExecutionProvider::kOpenVINO;
    } else if (ep == "nvtensorrtrtx" ||
               ep == "NvTensorRtRtx" ||
               ep == "NvTensorRTRTXExecutionProvider") {
      return ExecutionProvider::kTensorRT_RTX;
    } else if (ep == "vitisai" ||
               ep == "VitisAI" ||
               ep == "VitisAIExecutionProvider") {
      return ExecutionProvider::kVitisAI;
    } else if (ep == "RyzenAI") {
      return ExecutionProvider::kRyzenAI;
    } else if (ep == "qnn" ||
               ep == "QNN" ||
               ep == "QNNExecutionProvider") {
      return ExecutionProvider::kQNN;
    } else {
      return ExecutionProvider::kUnknown;
    }
  }

  /// Convert an ExecutionProvider enum to the GenAI provider name
  /// See NormalizeProviderName https://github.com/microsoft/onnxruntime-genai/blob/main/src/config.cpp
  static std::string_view EPtoGenAI(ExecutionProvider ep) {
    switch (ep) {
      case ExecutionProvider::kCUDA:
        return "cuda";
      case ExecutionProvider::kWebGPU:
        return "WebGPU";
      case ExecutionProvider::kOpenVINO:
        return "OpenVINO";
      case ExecutionProvider::kTensorRT_RTX:
        return "NvTensorRtRtx";
      case ExecutionProvider::kVitisAI:
        return "VitisAI";
      case ExecutionProvider::kRyzenAI:
        return "RyzenAI";
      case ExecutionProvider::kQNN:
        return "QNN";
      default:
        return "";
    }
  }

  /// CPU and default use GenAI's implicit provider; only mapped EPs have an explicit provider name.
  static bool HasExplicitGenAIProvider(ExecutionProvider ep) {
    return !EPtoGenAI(ep).empty();
  }

  /// Convert an ExecutionProvider enum to the ORT registration name.
  /// e.g. kCUDA → "CUDAExecutionProvider"
  static std::string_view EPtoRegistrationName(ExecutionProvider ep) {
    switch (ep) {
      case ExecutionProvider::kCPU:
        return "CPUExecutionProvider";
      case ExecutionProvider::kCUDA:
        return "CUDAExecutionProvider";
      case ExecutionProvider::kWebGPU:
        return "WebGpuExecutionProvider";
      case ExecutionProvider::kOpenVINO:
        return "OpenVINOExecutionProvider";
      case ExecutionProvider::kTensorRT_RTX:
        return "NvTensorRTRTXExecutionProvider";
      case ExecutionProvider::kVitisAI:
        return "VitisAIExecutionProvider";
      case ExecutionProvider::kRyzenAI:
        return "RyzenAIExecutionProvider";
      case ExecutionProvider::kQNN:
        return "QNNExecutionProvider";
      default:
        return "";
    }
  }
};

}  // namespace fl
