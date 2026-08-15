// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/cancellable.h"
#include "inferencing/session/request.h"

#include <ort_genai.h>

#include <stdexcept>

namespace fl {

/// Some inference paths drive OgaGenerator directly and have no ICancellable implementation.
/// This non-owning adapter exposes those engine calls to request cancellation; the generator must outlive it.
class OgaGeneratorCancellable : public ICancellable {
 public:
  explicit OgaGeneratorCancellable(OgaGenerator& generator) : generator_(generator) {}

  void Cancel() override { generator_.SetRuntimeOption("terminate_session", "1"); }

 private:
  OgaGenerator& generator_;
};

/// Return false when cancellation or timeout causes ORT GenAI to throw; rethrow other runtime errors.
/// Templated to support fake generators in unit tests.
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
