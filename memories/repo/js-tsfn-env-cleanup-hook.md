# JS addon: release ThreadSafeFunction from an env cleanup hook, never a finalizer

A `Napi::ThreadSafeFunction` (TSFN) held for the life of the addon — e.g. stored
in `SetInstanceData` addon data — must be released via
`napi_add_env_cleanup_hook` (node-addon-api: `env.AddCleanupHook(...)`), **not**
from the instance-data destructor (`~AddonData`) or an `ObjectWrap` finalizer.

Per Node's N-API docs, *"Finalization on the exit of the Node.js environment"*:
the `napi_finalize` callbacks of JS objects, thread-safe functions, and
environment instance data are *"invoked immediately and independently"* at env
teardown, and *"scheduled after the manually registered cleanup hooks."*
Releasing a TSFN from a finalizer therefore races the TSFN's own `uv_async`
handle close against libuv/V8 graceful teardown, and Node dereferences freed
state while draining the loop.

**Symptom:** intermittent process-exit crash after all app work completes —
`0xC0000005` (access violation) faulting in `node.exe` inside
`node::FreeEnvironment` → `uv_run` (reading a freed object, e.g. `read of 0xc`),
occasionally `0xC0000374` (heap corruption). Variable hit-rate, worse on
Node 24+. WER (AeDebug JIT and LocalDumps) cannot capture it because the fault
is consumed first-chance during teardown; a live debugger hides it (Heisenbug).
A first-chance vectored exception handler (`AddVectoredExceptionHandler`) is the
tool that names the faulting frame.

**Rule:** for a process-lifetime TSFN, register a cleanup hook at `Init` that
does the `Release()`, and null the handle so the destructor does not
double-release. Example (`sdk_v2/js/native/src/addon.cc`,
`AddonData::buffer_release_tsfn`):

```cpp
data->buffer_release_tsfn = Napi::ThreadSafeFunction::New(env, noop, name, 0, 1);
data->buffer_release_tsfn.Unref(env);  // don't pin the loop
env.AddCleanupHook(
    [](AddonData* d) {
      if (d->buffer_release_tsfn) {
        d->buffer_release_tsfn.Release();
        d->buffer_release_tsfn = Napi::ThreadSafeFunction();  // null so ~AddonData won't re-release
      }
    },
    data);
```

This was the true root cause of the JS SDK's Node-exit crash. Earlier
attributions (a libcurl connection-pool cleanup thread; ORT/GenAI `OgaShutdown`
static teardown) were misdiagnoses driven by the variable hit-rate — neither was
the cause, and `process.exit()` only masked it. The proper fix needs no
`process.exit()` and no native shutdown hook.
