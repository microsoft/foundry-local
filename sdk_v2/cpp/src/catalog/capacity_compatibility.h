#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace fl {

struct DeviceCapacity {
  std::optional<uint64_t> total_vram_bytes;
};

struct ModelCapacityRequirements {
  std::optional<uint64_t> minimum_vram_bytes;
};

inline bool MeetsCapacityRequirements(bool is_gpu, const ModelCapacityRequirements& requirements,
                                      std::span<const DeviceCapacity> devices) {
  if (!is_gpu || !requirements.minimum_vram_bytes || devices.empty()) {
    return true;
  }
  for (const auto& device : devices) {
    if (!device.total_vram_bytes || *device.total_vram_bytes >= *requirements.minimum_vram_bytes) {
      return true;
    }
  }
  return false;
}

}  // namespace fl