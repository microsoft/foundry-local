# Web-Service Shutdown Must Cancel In-Flight Generations Before Joining Threads

`WebService::Stop()` hard-`join()`s streaming threads via `StreamingThreadTracker::JoinAll()` with **no**
cancellation signal of its own. A chat streaming thread grinding in ORT GenAI token generation only stops when
`Request.canceled` (a `mutable std::atomic<bool>`, polled every token by the generation loop at
`chat_session.cc` `while (... && !request.canceled)`) is set. So **cancellation must be signalled before the
join**, or shutdown deadlocks and the (in-process) host process never exits.

The contract, wired across three sites — keep them in sync:

- `Session::Cancel()` (`inferencing/session/session.h/.cc`) flips the cancel flag on every in-flight request.
  `Session` tracks live requests in `active_requests_` (an `unordered_set<const Request*>` guarded by its own
  `active_requests_mutex_`, **not** `request_mutex_`, because concurrent sessions like audio hold several and
  `Cancel()` must not wait on an active generation). `ProcessRequest` registers/deregisters via an RAII guard so a
  throwing `ProcessRequestImpl` can't leave a dangling `Request*`. `Cancel()` **only sets atomics** — never blocks,
  never joins — so it is safe to call while `SessionManager` holds its lock.
- `SessionManager::CancelAll()` (`inferencing/session/session_manager.cc`) iterates `sessions_` and calls
  `s->Cancel()` on each (under `mutex_`). It is **not** a stub — do not revert it to the old "Future (Phase 3)"
  no-op.
- `Manager::Shutdown()` (`manager.cc`) order is load-bearing: `RejectNewLoads()` → **`CancelAll()`** →
  `StopWebService()` → `WaitForDrain()` → `UnloadAll()`. `CancelAll()` **must precede** `StopWebService()`
  (which calls `JoinAll()`). Reordering these re-introduces the deadlock.

`SessionManagerCancelTest.CancelAllCancelsInFlightRequestsOnEverySession` (model-free, in
`test/internal_api/session_manager_test.cc`) asserts `CancelAll()` unblocks in-flight requests, but the
`Manager::Shutdown` **ordering** is not directly unit-covered — treat the order as a contract.

Not yet fixed (separate defect): a **client disconnect mid-stream** does not reliably set `request.canceled`. The
streaming callback (`CallbackHandler::WorkerLoop` → `fn_`) only enqueues items; the actual socket write happens in
oatpp's connection worker, so a dropped client isn't propagated back to the callback's return value. Wiring
disconnect→cancel needs an oatpp-level signal (SSE body / connection state). Catalog `maxOutputTokens`
(`FOUNDRY_LOCAL_MODEL_PROP_MAX_OUTPUT_TOKENS_INT`) is a **ceiling**, not a per-request default — `chat_session.cc`
clamps to it but it does not shorten a degenerate run whose model ceiling is >= the 2048 default.
