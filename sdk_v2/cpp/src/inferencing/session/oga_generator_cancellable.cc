// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "inferencing/session/oga_generator_cancellable.h"

#include <ort_genai.h>

namespace fl {

bool OgaGeneratorCancellable::Cancel() noexcept {
  // Engine-level termination interrupts an in-flight compute (e.g. a long encoder pass),
  // which a between-token flag check cannot do. Mirrors OnnxAudioGenerator::Cancel.
  try {
    generator_.SetRuntimeOption("terminate_session", "1");
    return true;
  } catch (...) {
    // SetRuntimeOption may not be supported by all ORT GenAI builds.
    return false;
  }
}

}  // namespace fl
