// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/invocation_context.h"

#include <cstdint>
#include <cstdio>
#include <random>

namespace fl {

std::string MakeGuidV4Hex() {
  // std::random_device + mt19937_64 is enough here — this is a correlation /
  // session id, not a cryptographic identifier, so the OS UUID API is overkill.
  std::random_device rd;
  std::mt19937_64 gen{(static_cast<uint64_t>(rd()) << 32) | rd()};
  uint64_t hi = gen();
  uint64_t lo = gen();

  // Set version (4) and variant (10xx) bits.
  hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
  lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

  char buf[37];
  std::snprintf(buf, sizeof(buf),
                "%08x-%04x-%04x-%04x-%012llx",
                static_cast<unsigned>((hi >> 32) & 0xFFFFFFFFu),
                static_cast<unsigned>((hi >> 16) & 0xFFFFu),
                static_cast<unsigned>(hi & 0xFFFFu),
                static_cast<unsigned>((lo >> 48) & 0xFFFFu),
                static_cast<unsigned long long>(lo & 0x0000FFFFFFFFFFFFULL));
  return std::string(buf);
}

}  // namespace fl
