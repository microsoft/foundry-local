#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace fl {

struct DeviceCapacity {
  std::optional<uint64_t> total_vram_bytes;
  bool operator==(const DeviceCapacity&) const = default;
};

struct ExecutionTarget {
  std::string provider;
  uint32_t device_id = 0;
  std::string device_type;
  DeviceCapacity capacity;
  std::map<std::string, std::string> options = {};
  bool operator==(const ExecutionTarget&) const = default;
};

}  // namespace fl