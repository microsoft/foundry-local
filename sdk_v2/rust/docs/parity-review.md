# SDK V2 binding parity — design notes

This document records the non-obvious design decisions behind the SDK V2 Rust
binding and its alignment with the C#, JavaScript, and Python bindings. It is an
architecture-decision record, not a changelog: it captures the *why* that is not
evident from the code, so the reasoning survives after the originating pull
request is out of view. For the list of what changed, see the PR history.

## Typed-session task validation lives in every binding by design

`ChatSession`, `EmbeddingsSession`, and `AudioSession` each validate the model's
task at construction — C# throws `ArgumentException`, Python raises `ValueError`,
JavaScript throws `TypeError`, and Rust returns `FoundryLocalError::Validation`.
This looks redundant with native validation, but it is not, and it must not be
"cleaned up" by deferring solely to native.

There is a single native entry point, `Session_Create(model)`, which dispatches
the concrete session type from the model's task. However, it first requires the
model to already be loaded and throws `FOUNDRY_LOCAL_ERROR_INVALID_USAGE`
("model must be loaded before creating a session") *before* it ever inspects the
task. So natively:

- A wrong-task model that has **not** been loaded fails with a generic
  "not loaded" error, not a task-mismatch error.
- The task-dispatch `FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT` only surfaces for a
  *wholly unrecognized* task.

The model's task, by contrast, is catalog metadata available **without** loading.
Validating it in the binding surfaces a precise, task-specific error regardless
of load state, and keeps that error identical whether or not the model happens to
be loaded. Moving the check exclusively to native would require deferring the
model-load requirement inside `Session_Create` — a larger change with its own
complications — so the binding-level check is the intended design.

Compatibility is additionally enforced at use time as a backstop: unsupported
input items and type-specific operations (for example `UndoTurns` on a non-chat
session) fail with `FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT` /
`FOUNDRY_LOCAL_ERROR_INVALID_USAGE`.

## Native error identity diverges deliberately from the v1 Rust SDK

The v2 Rust binding exposes a `FoundryLocalError::Native { code, message }`
variant carrying a `NativeErrorCode`, queryable via `native_code()` /
`native_message()`, so callers can distinguish cancellation, network failures,
invalid arguments, invalid usage, unsupported operations, and internal failures
without matching error strings.

This is a deliberate departure from the v1 Rust SDK, which had no native
error-code concept and mapped native failures onto categorical variants
(`CommandExecution`, `Validation`, `ModelOperation`, `Internal`) by inspecting
message strings. The categorical variants remain in the v2 enum for SDK-side
(non-native) failures such as invalid configuration and input validation.

All v2 bindings surface the stable native code so callers can branch on it
without string matching: Rust via `FoundryLocalError::Native { code, .. }`,
JavaScript by tagging errors with a numeric `code` (and
`name === "FoundryLocalError"`), Python via
`FoundryLocalException(message, error_code=...)`, and C# via a nullable
`FoundryLocalException.ErrorCode` (a public `FoundryLocalErrorCode` enum; `null`
for SDK-side failures that have no native code). C# continues to translate the
cancellation code to `OperationCanceledException` as is idiomatic on .NET.

## Conversion failures are propagated, not swallowed

Native item conversion returns `Result<Item>` rather than using `None` for both
an absent item and a conversion failure. An unknown item kind generally means the
Rust binding is older than the native ABI; reporting a successful but truncated
response would silently hide data loss, so conversion failures propagate through
response getters, streaming callbacks, item queues, and usage retrieval instead.

## The Rust FFI vtables are maintained by hand

The Rust FFI vtables in `src/detail/ffi.rs` mirror the C ABI function-pointer
tables in `foundry_local_c.h` and are maintained manually. Slot order must match
the header exactly. Any newly appended native function must be added to the Rust
vtable in the same change; a mismatch shifts every subsequent slot and corrupts
dispatch. Consider generating or mechanically checking these tables to prevent
future drift.