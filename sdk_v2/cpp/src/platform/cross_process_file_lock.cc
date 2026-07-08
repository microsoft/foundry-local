// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
// Cross-platform orchestration for CrossProcessFileLock. The platform-specific
// pieces — the lock handle (State) and its releasing destructor,
// FormatProcessInfo, the CrossProcessFileLock destructor/constructor, and
// TryAcquireForDirectory — live in
// src/platform/{windows,posix}/cross_process_file_lock.cc.
#include "platform/cross_process_file_lock.h"
#include "exception.h"

#include <foundry_local/foundry_local_c.h>

#include <chrono>
#include <thread>

namespace fl {

std::unique_ptr<CrossProcessFileLock> CrossProcessFileLock::WaitForDirectoryLock(
    const std::filesystem::path& directory,
    const CancellationPredicate& is_cancelled,
    ILogger& logger,
    std::chrono::milliseconds poll_interval,
    std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  // `is_cancelled` is the caller's progress callback, which also serves as the
  // liveness heartbeat — it emits 0% on every invocation. We therefore poll it
  // on a single cadence (once per `poll_interval`) rather than on a separate
  // fast cancellation tick: a faster tick would spam the user callback (~10x/s)
  // for the entire wait, and cancelling a multi-minute cross-process wait a
  // second sooner is imperceptible. There is no separate cancellation channel
  // to decouple the heartbeat from.
  while (true) {
    if (is_cancelled && is_cancelled()) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED, "lock acquisition cancelled");
    }
    auto lock = CrossProcessFileLock::TryAcquireForDirectory(directory, logger);
    if (lock) {
      return lock;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      FL_THROW(FOUNDRY_LOCAL_ERROR_INTERNAL,
               "timed out waiting for cross-process download lock on '" + directory.string() + "'");
    }
    std::this_thread::sleep_for(poll_interval);
  }
}

}  // namespace fl
