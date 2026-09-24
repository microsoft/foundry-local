# Python native lifetime protocol

- `ModelInfoBuilder` leases its native pointer under its instance `RLock`; every native getter, setter, and the entire
  `Catalog.register_model` call must hold that lease. `close()` invalidates and releases the pointer under the same lock.
- `Session` registers itself with its owning `FoundryLocalManager` after native creation and retains that manager until
  `Session_Release` completes. Every session ABI operation holds the session operation lock and manager `_native_call`.
- Manager close atomically rejects new calls, waits for admitted calls, closes live sessions, and only then calls
  `Manager_Shutdown` followed by `Manager_Release` after all session registrations have drained.
- Calling manager or session close reentrantly from one of its own active native calls raises instead of deadlocking.