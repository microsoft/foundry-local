// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "telemetry/invocation_context.h"

#include "version.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <random>
#include <thread>

namespace fl {

namespace {

uint64_t MakeNonThrowingSeed() {
  try {
    std::random_device rd;
    return (static_cast<uint64_t>(rd()) << 32) ^ rd();
  } catch (...) {
    static std::atomic<uint64_t> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto sequence = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto thread_id = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return static_cast<uint64_t>(now) ^ (sequence * 0x9E3779B97F4A7C15ULL) ^
           static_cast<uint64_t>(thread_id);
  }
}

std::string& MutableDefaultUserAgent() {
  static std::string user_agent = std::string("foundry-local-core/") + FOUNDRY_LOCAL_VERSION;
  return user_agent;
}

}  // namespace

std::string DefaultUserAgent() {
  return MutableDefaultUserAgent();
}

void SetDefaultUserAgent(std::string user_agent) {
  if (user_agent.empty()) {
    user_agent = std::string("foundry-local-core/") + FOUNDRY_LOCAL_VERSION;
  }
  MutableDefaultUserAgent() = std::move(user_agent);
}

std::string GenerateGuidV4() {
  uint64_t hi;
  uint64_t lo;
  try {
    // Match ORT: source all 128 bits directly from random_device instead of expanding a smaller PRNG seed.
    std::random_device rd;
    hi = (static_cast<uint64_t>(rd()) << 32) | rd();
    lo = (static_cast<uint64_t>(rd()) << 32) | rd();
  } catch (...) {
    // Telemetry identifiers must not make SDK initialization fail on platforms without random_device.
    std::mt19937_64 fallback{MakeNonThrowingSeed()};
    hi = fallback();
    lo = fallback();
  }

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
