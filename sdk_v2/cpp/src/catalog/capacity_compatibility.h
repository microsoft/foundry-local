#pragma once

#include "ep_detection/execution_target.h"
#include "inferencing/execution_provider.h"
#include "model_info.h"
#include "utils.h"

#include <cstdint>
#include <optional>
#include <span>
#include <sstream>
#include <vector>

namespace fl {

struct ModelCapacityRequirements {
  std::optional<uint64_t> minimum_vram_bytes;
};

enum class CompatibilityState { kFits, kDoesNotFit, kUnknown };
enum class CompatibilityReason { kSufficientCapacity, kInsufficientCapacity, kMissingRequirement, kUnknownCapacity };

struct CompatibilityResult {
  CompatibilityState state;
  CompatibilityReason reason;
  std::vector<ExecutionTarget> eligible_targets;
};

inline bool HasNonTextModalities(const ModelInfo& info) {
  for (const auto* key : {FOUNDRY_LOCAL_MODEL_PROP_INPUT_MODALITIES_STR, FOUNDRY_LOCAL_MODEL_PROP_OUTPUT_MODALITIES_STR}) {
    if (const auto* value = info.GetPropertyStr(key)) {
      std::istringstream modalities(*value);
      std::string modality;
      while (std::getline(modalities, modality, ',')) {
        modality = ToLower(Trim(modality));
        if (!modality.empty() && modality != "text") {
          return true;
        }
      }
    }
  }

  return false;
}

inline CompatibilityResult EvaluateCapacity(bool is_gpu, const ModelCapacityRequirements& requirements,
                                            std::span<const ExecutionTarget> targets) {
  CompatibilityResult result{CompatibilityState::kUnknown, CompatibilityReason::kUnknownCapacity, {}};
  if (!is_gpu || !requirements.minimum_vram_bytes) {
    result.reason = CompatibilityReason::kMissingRequirement;
    result.eligible_targets.assign(targets.begin(), targets.end());
    return result;
  }
  bool unknown = targets.empty();
  bool fits = false;
  for (const auto& target : targets) {
    if (target.device_type != "GPU" || !target.capacity.total_vram_bytes) {
      unknown = true;
      result.eligible_targets.push_back(target);
    } else if (*target.capacity.total_vram_bytes >= *requirements.minimum_vram_bytes) {
      fits = true;
      result.eligible_targets.push_back(target);
    }
  }
  if (fits) {
    result.state = CompatibilityState::kFits;
    result.reason = CompatibilityReason::kSufficientCapacity;
    std::stable_partition(result.eligible_targets.begin(), result.eligible_targets.end(), [&](const auto& target) {
      return target.device_type == "GPU" && target.capacity.total_vram_bytes &&
             *target.capacity.total_vram_bytes >= *requirements.minimum_vram_bytes;
    });
  } else if (!unknown) {
    result.state = CompatibilityState::kDoesNotFit;
    result.reason = CompatibilityReason::kInsufficientCapacity;
  }
  return result;
}

inline CompatibilityResult EvaluateProviderCapacity(bool is_gpu, const ModelCapacityRequirements& requirements,
                                                    std::span<const ExecutionTarget> targets,
                                                    std::string_view required_provider, bool generic_gpu = false,
                                                    bool is_multimodal = false) {
  const auto normalize = [](std::string_view provider) {
    return ToLower(EPUtils::NormalizeRegistrationName(std::string(provider)));
  };
  std::vector<ExecutionTarget> candidates;
  const auto append_provider = [&](std::string_view provider) {
    for (const auto& target : targets) {
      if (normalize(target.provider) == normalize(provider)) {
        candidates.push_back(target);
      }
    }
  };
  if (generic_gpu) {
    if (!is_multimodal) {
      append_provider("CUDAExecutionProvider");
    }
    append_provider("WebGpuExecutionProvider");
  }
  if (candidates.empty()) {
    append_provider(required_provider);
  }
  return EvaluateCapacity(is_gpu, requirements, candidates);
}

inline bool MeetsCapacityRequirements(bool is_gpu, const ModelCapacityRequirements& requirements,
                                      std::span<const DeviceCapacity> devices) {
  std::vector<ExecutionTarget> targets;
  for (const auto& device : devices) {
    targets.push_back({{}, 0, "GPU", device});
  }
  return EvaluateCapacity(is_gpu, requirements, targets).state != CompatibilityState::kDoesNotFit;
}

}  // namespace fl