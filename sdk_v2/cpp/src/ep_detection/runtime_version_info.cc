// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "ep_detection/runtime_version_info.h"

#include "logger.h"
#include "version.h"

#include <onnxruntime_c_api.h>

namespace fl {

std::string GetEpVersion(const OrtApi& ort_api, OrtEnv& ort_env, const std::string& ep_name) {
  const OrtEpDevice* const* ep_devices = nullptr;
  size_t num_devices = 0;
  OrtStatus* status = ort_api.GetEpDevices(&ort_env, &ep_devices, &num_devices);

  if (status != nullptr) {
    ort_api.ReleaseStatus(status);
    return "unknown";
  }

  for (size_t i = 0; i < num_devices; ++i) {
    const OrtEpDevice* device = ep_devices[i];
    const char* name = ort_api.EpDevice_EpName(device);
    if (!name || ep_name != name) {
      continue;
    }

    const OrtKeyValuePairs* metadata = ort_api.EpDevice_EpMetadata(device);
    if (!metadata) {
      return "unknown";
    }

    const char* version = ort_api.GetKeyValue(metadata, "version");
    return version ? std::string(version) : "unknown";
  }

  return "unknown";
}

void LogRuntimeVersions(ILogger& logger) {
  const OrtApiBase* api_base = OrtGetApiBase();
  const char* ort_version = api_base ? api_base->GetVersionString() : nullptr;
  logger.Log(LogLevel::Information,
             std::string("Runtime versions: onnxruntime=") + (ort_version ? ort_version : "unknown") +
                 ", onnxruntime-genai=" + FOUNDRY_LOCAL_ORT_GENAI_VERSION);
}

}  // namespace fl
