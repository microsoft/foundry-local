// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include <cstddef>
#include <string>

namespace fl {

class ILogger;

// -----------------------------------------------------------------------------
// Inference divergence diagnostics.
//
// These helpers exist to narrow down why identical model weights + identical
// pinned runtime versions can still produce different generated tokens across
// environments (e.g. CI vs. a developer box). They split the problem space into
// two independent axes:
//
//   * Machine / hardware specific  -> LogHardwareFingerprint
//   * Model / weights specific     -> LogModelWeightsFingerprint
//
// plus an opt-in per-token trace (with top-2 logit margins) that reveals the
// exact step at which two runs diverge and whether the winning token was a
// near-tie (the classic symptom of CPU-microarchitecture FP nondeterminism
// flipping a greedy argmax).
// -----------------------------------------------------------------------------

/// Logs a machine/hardware fingerprint: CPU brand string, detected SIMD feature
/// flags (SSE/AVX/AVX2/AVX512F/FMA), pointer width and hardware concurrency.
/// Compare this across environments to decide whether a divergence is
/// hardware/microarchitecture specific.
void LogHardwareFingerprint(ILogger& logger);

/// Logs a model/weights fingerprint: the resolved model directory and the
/// SHA256 of each weight/config artifact found under it (model.onnx,
/// model.onnx.data, genai_config.json, tokenizer files, ...). Identical hashes
/// across environments prove the divergence is NOT caused by the model bits.
void LogModelWeightsFingerprint(ILogger& logger, const std::string& model_path);

/// True when per-token generation tracing is enabled via the environment
/// variable FOUNDRY_LOCAL_DEBUG_TOKENS=1. Kept cheap so callers can guard the
/// (relatively expensive) logit inspection behind it.
bool DebugTokenTraceEnabled();

/// Top-2 greedy-decode candidates for a single generation step, derived from
/// the raw logits tensor. `margin` (= top1 - top2) approaching zero indicates a
/// near-tie that is prone to flipping under different FP rounding.
struct TokenTop2 {
  bool valid = false;
  int top1_id = -1;
  int top2_id = -1;
  float top1 = 0.0f;
  float top2 = 0.0f;
  float margin = 0.0f;
};

/// Computes the top-2 candidates from a float32 logits buffer for the final
/// position. Returns {valid=false} when the buffer is empty. `float16`/other
/// element types should be converted by the caller (or skipped).
TokenTop2 ComputeTop2FromLogits(const float* logits, std::size_t vocab_size);

/// Emits one per-token trace line to stderr (id + decoded text, plus the top-2
/// logit margin when available). No-op unless DebugTokenTraceEnabled().
void DebugTraceToken(int step, int chosen_token_id, const std::string& text, const TokenTop2& top2);

}  // namespace fl
