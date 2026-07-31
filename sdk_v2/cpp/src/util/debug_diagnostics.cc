// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "util/debug_diagnostics.h"

#include "logger.h"
#include "util/sha256.h"

#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace fl {

namespace {

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define FL_DEBUG_X86 1
#else
#define FL_DEBUG_X86 0
#endif

#if FL_DEBUG_X86
// Wrapper over the compiler-specific CPUID intrinsic. `leaf`/`subleaf` select
// the CPUID query; results land in regs[0..3] = {eax, ebx, ecx, edx}.
void CpuId(std::uint32_t leaf, std::uint32_t subleaf, std::uint32_t regs[4]) {
#if defined(_MSC_VER)
  int out[4];
  __cpuidex(out, static_cast<int>(leaf), static_cast<int>(subleaf));
  regs[0] = static_cast<std::uint32_t>(out[0]);
  regs[1] = static_cast<std::uint32_t>(out[1]);
  regs[2] = static_cast<std::uint32_t>(out[2]);
  regs[3] = static_cast<std::uint32_t>(out[3]);
#else
  unsigned int a = 0, b = 0, c = 0, d = 0;
  __get_cpuid_count(leaf, subleaf, &a, &b, &c, &d);
  regs[0] = a;
  regs[1] = b;
  regs[2] = c;
  regs[3] = d;
#endif
}

std::string CpuBrand() {
  std::uint32_t regs[4];
  CpuId(0x80000000u, 0, regs);
  if (regs[0] < 0x80000004u) {
    return "unknown";
  }

  std::array<char, 49> brand{};
  for (std::uint32_t i = 0; i < 3; ++i) {
    CpuId(0x80000002u + i, 0, regs);
    std::memcpy(brand.data() + i * 16, regs, 16);
  }
  brand[48] = '\0';

  // Trim leading spaces that some CPUs pad the brand string with.
  std::string s(brand.data());
  const auto first = s.find_first_not_of(' ');
  return first == std::string::npos ? s : s.substr(first);
}

std::string CpuSimdFlags() {
  std::vector<std::string> flags;
  std::uint32_t regs[4];

  CpuId(1u, 0, regs);
  const std::uint32_t ecx1 = regs[2];
  const std::uint32_t edx1 = regs[3];
  if (edx1 & (1u << 25)) flags.push_back("sse");
  if (edx1 & (1u << 26)) flags.push_back("sse2");
  if (ecx1 & (1u << 0)) flags.push_back("sse3");
  if (ecx1 & (1u << 19)) flags.push_back("sse4.1");
  if (ecx1 & (1u << 20)) flags.push_back("sse4.2");
  if (ecx1 & (1u << 12)) flags.push_back("fma");
  if (ecx1 & (1u << 28)) flags.push_back("avx");

  CpuId(7u, 0, regs);
  const std::uint32_t ebx7 = regs[1];
  if (ebx7 & (1u << 5)) flags.push_back("avx2");
  if (ebx7 & (1u << 16)) flags.push_back("avx512f");
  if (ebx7 & (1u << 30)) flags.push_back("avx512bw");
  if (ebx7 & (1u << 31)) flags.push_back("avx512vl");

  if (flags.empty()) {
    return "none";
  }

  std::string out;
  for (std::size_t i = 0; i < flags.size(); ++i) {
    if (i) out += ",";
    out += flags[i];
  }
  return out;
}
#endif  // FL_DEBUG_X86

}  // namespace

void LogHardwareFingerprint(ILogger& logger) {
#if FL_DEBUG_X86
  const std::string brand = CpuBrand();
  const std::string simd = CpuSimdFlags();
#else
  const std::string brand = "non-x86";
  const std::string simd = "n/a";
#endif

  logger.Log(LogLevel::Information,
             fmt::format("HW fingerprint: cpu='{}' simd=[{}] ptr_bits={} hw_threads={}",
                         brand, simd, static_cast<int>(sizeof(void*) * 8),
                         std::thread::hardware_concurrency()));
}

void LogModelWeightsFingerprint(ILogger& logger, const std::string& model_path) {
  namespace fs = std::filesystem;

  std::error_code ec;
  if (!fs::exists(model_path, ec) || !fs::is_directory(model_path, ec)) {
    logger.Log(LogLevel::Warning,
               fmt::format("Weights fingerprint: model path '{}' not found", model_path));
    return;
  }

  // Hash the artifacts that determine inference output. The big weight blob
  // (model.onnx.data) is the decisive one; the rest catch config/tokenizer
  // drift. Files that don't exist for a given model are silently skipped.
  static constexpr std::array<const char*, 5> kArtifacts = {
      "model.onnx", "model.onnx.data", "genai_config.json", "tokenizer.json", "config.json"};

  logger.Log(LogLevel::Information,
             fmt::format("Weights fingerprint: model_path='{}'", model_path));

  for (const char* name : kArtifacts) {
    const fs::path p = fs::path(model_path) / name;
    if (!fs::exists(p, ec)) {
      continue;
    }

    const auto size = fs::file_size(p, ec);
    const std::string hash = Sha256File(p);
    logger.Log(LogLevel::Information,
               fmt::format("  weight[{}] size={} sha256={}", name,
                           ec ? -1 : static_cast<long long>(size),
                           hash.empty() ? "unavailable" : hash));
  }
}

bool DebugTokenTraceEnabled() {
  static const bool enabled = []() {
    const char* v = std::getenv("FOUNDRY_LOCAL_DEBUG_TOKENS");
    return v != nullptr && v[0] == '1';
  }();
  return enabled;
}

TokenTop2 ComputeTop2FromLogits(const float* logits, std::size_t vocab_size) {
  TokenTop2 r;
  if (logits == nullptr || vocab_size == 0) {
    return r;
  }

  int top1_id = 0;
  int top2_id = -1;
  float top1 = logits[0];
  float top2 = -std::numeric_limits<float>::infinity();

  for (std::size_t i = 1; i < vocab_size; ++i) {
    const float v = logits[i];
    if (v > top1) {
      top2 = top1;
      top2_id = top1_id;
      top1 = v;
      top1_id = static_cast<int>(i);
    } else if (v > top2) {
      top2 = v;
      top2_id = static_cast<int>(i);
    }
  }

  r.valid = true;
  r.top1_id = top1_id;
  r.top2_id = top2_id;
  r.top1 = top1;
  r.top2 = top2;
  r.margin = top1 - top2;
  return r;
}

void DebugTraceToken(int step, int chosen_token_id, const std::string& text, const TokenTop2& top2) {
  if (!DebugTokenTraceEnabled()) {
    return;
  }

  // Escape newlines so each token stays on a single grep-able line.
  std::string escaped;
  escaped.reserve(text.size());
  for (char c : text) {
    if (c == '\n') {
      escaped += "\\n";
    } else if (c == '\r') {
      escaped += "\\r";
    } else {
      escaped += c;
    }
  }

  if (top2.valid) {
    std::fprintf(stderr,
                 "[token-trace] step=%d id=%d text='%s' top1=(%d,%.6f) top2=(%d,%.6f) margin=%.6f\n",
                 step, chosen_token_id, escaped.c_str(), top2.top1_id, top2.top1, top2.top2_id,
                 top2.top2, top2.margin);
  } else {
    std::fprintf(stderr, "[token-trace] step=%d id=%d text='%s'\n", step, chosen_token_id,
                 escaped.c_str());
  }
  std::fflush(stderr);
}

}  // namespace fl
