// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

namespace fl {

/// Anything that can be interrupted from a thread other than the one driving it.
///
/// Implemented by the per-request generators (chat, audio). A generation loop only
/// observes `Request::ShouldStop()` between tokens, which is not enough on its own:
/// a long prefill or a single slow decode step can block inside the ORT GenAI engine
/// for an unbounded time. Cancel() reaches into the engine (terminate_session) so an
/// external canceller or an expired deadline interrupts mid-compute.
///
/// Cancel() must be safe to call from any thread, at any point in the generator's
/// lifetime, and must be idempotent.
class ICancellable {
 public:
  virtual ~ICancellable() = default;

  virtual void Cancel() = 0;
};

}  // namespace fl
