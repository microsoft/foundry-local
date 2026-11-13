// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

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
  kAMDGPU = 10,
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
      std::string normalized(ep);
      std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      for (const auto provider : {ExecutionProvider::kCPU, ExecutionProvider::kCUDA,
                                 ExecutionProvider::kWebGPU, ExecutionProvider::kOpenVINO,
                                 ExecutionProvider::kTensorRT_RTX, ExecutionProvider::kVitisAI,
                                 ExecutionProvider::kRyzenAI, ExecutionProvider::kQNN,
                                 ExecutionProvider::kAMDGPU}) {
        for (const auto name : {EPtoRegistrationName(provider), EPtoGenAI(provider)}) {
          std::string candidate(name);
          std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
          if (!candidate.empty() && normalized == candidate) return provider;
        }
      }

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
      case ExecutionProvider::kAMDGPU:
        return "AMDGPU";
      default:
        return "";
    }
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
      case ExecutionProvider::kAMDGPU:
        return "AMDGPUExecutionProvider";
      default:
        return "";
    }
  }

  static std::string NormalizeRegistrationName(std::string_view provider) {
    const auto ep = StringtoEP(provider);
    if (ep != ExecutionProvider::kUnknown) {
      return std::string(EPtoRegistrationName(ep));
    }

    if (provider == "dml" || provider == "DML" || provider == "DMLExecutionProvider") {
      return "DmlExecutionProvider";
    }

    return std::string(provider);
  }

  static std::string_view RequiredEpForModelId(std::string_view model_id) {
    constexpr struct {
      std::string_view model_id_substr;
      std::string_view required_ep;
    } requirements[] = {
        {"cuda-gpu", "CUDAExecutionProvider"},
        {"openvino-npu", "OpenVINOExecutionProvider"},
        {"openvino-gpu", "OpenVINOExecutionProvider"},
        {"qnn-npu", "QNNExecutionProvider"},
        {"trtrtx-gpu", "NvTensorRTRTXExecutionProvider"},
        {"vitis-npu", "VitisAIExecutionProvider"},
    };
    for (const auto& requirement : requirements) {
      if (model_id.find(requirement.model_id_substr) != std::string_view::npos) {
        return requirement.required_ep;
      }
    }

    return {};
  }

  /// CPU needs no bootstrapper; DirectML is supplied by WinML.
  static std::string RequiredEpForConfigProvider(std::string_view provider) {
    const auto normalized = NormalizeRegistrationName(provider);
    if (normalized == "CPUExecutionProvider" || normalized == "DmlExecutionProvider") {
      return {};
    }

    return normalized.ends_with("ExecutionProvider") ? normalized : std::string{};
  }

  /// Telemetry names the effective provider, while runtime mappings must preserve GenAI's empty/default sentinel.
  /// An explicit selection overrides the model config; otherwise an empty configured provider means implicit CPU.
  static std::string_view EPtoTelemetryName(ExecutionProvider ep, std::string_view configured_provider) {
    if (ep == ExecutionProvider::kDefault) {
      ep = configured_provider.empty() ? ExecutionProvider::kCPU : StringtoEP(configured_provider);
    }

    return EPtoRegistrationName(ep);
  }
};

}  // namespace fl
