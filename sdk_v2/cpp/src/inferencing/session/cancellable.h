// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#pragma once

namespace fl {

/// Anything that can be interrupted from a thread other than the one driving it.
///
/// Implemented by the per-request generators (chat, audio) and by the raw-OgaGenerator adapter. A generation
/// loop only observes the operation's stop latch between tokens, which is not enough on its own: a long
/// prefill or a single slow decode step can block inside the ORT GenAI engine for an unbounded time.
/// Cancel() reaches into the engine (terminate_session) so an external canceller or an expired deadline
/// interrupts mid-compute.
///
/// Cancel() must be safe to call from any thread, at any point in the generator's lifetime, and must be
/// idempotent. It returns true only when engine-level terminate_session was delivered successfully.
///
/// It is `noexcept` because it is invoked from framework boundaries that cannot handle a failure: a session
/// teardown loop cancelling every live operation, a watchdog thread whose exception would call
/// std::terminate anyway, and a generator-slot lock that must not unwind mid-iteration. Implementations
/// swallow engine-specific errors from terminate_session (not all ORT GenAI builds support it) rather than
/// letting cancellation failure escape.
class ICancellable {
 public:
  virtual ~ICancellable() = default;

  virtual bool Cancel() noexcept = 0;
};

}  // namespace fl
