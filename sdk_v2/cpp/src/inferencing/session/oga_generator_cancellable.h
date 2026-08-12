// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

#include "inferencing/session/cancellable.h"

#include <exception>

struct OgaGenerator;

namespace fl {

/// Adapts a raw OgaGenerator to ICancellable.
///
/// Some paths (streaming PCM transcription, Nemotron decode) drive an OgaGenerator
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

}  // namespace fl
