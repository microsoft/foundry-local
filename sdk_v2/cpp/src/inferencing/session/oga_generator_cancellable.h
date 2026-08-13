// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/cancellable.h"
#include "inferencing/session/request.h"

#include <stdexcept>

struct OgaGenerator;

namespace fl {

/// Adapts a raw OgaGenerator to ICancellable.
///
/// Some paths (embeddings, streaming PCM transcription, Nemotron decode) drive an OgaGenerator
/// directly instead of going through OnnxAudioGenerator, so they have no Cancel() of
/// their own. Wrapping the generator lets Session publish it for cancellation, which
/// is what makes those loops interruptible mid-compute rather than only between tokens.
///
/// Non-owning: the wrapped generator must outlive this adapter.
class OgaGeneratorCancellable : public ICancellable {
 public:
  explicit OgaGeneratorCancellable(OgaGenerator& generator) : generator_(generator) {}

  void Cancel() override;

 private:
  OgaGenerator& generator_;
};

/// Generate one token with a raw OGA generator.
///
/// ORT GenAI throws std::runtime_error when terminate_session interrupts an in-flight call. Treat that exception as
/// an expected stop only when the associated request was canceled or timed out; unrelated engine failures propagate.
/// This stays templated so OgaGenerator can remain forward-declared and the behavior can be tested without a model.
template <typename Generator>
[[nodiscard]] bool TryGenerateNextToken(Generator& generator, const Request& request) {
  try {
    generator.GenerateNextToken();
    return true;
  } catch (const std::runtime_error&) {
    if (request.ShouldStop()) {
      return false;
    }

    throw;
  }
}

}  // namespace fl
