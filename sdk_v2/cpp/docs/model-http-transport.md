# Custom model HTTP transports

Applications can supply the HTTP implementation for model acquisition. Foundry still owns the download coordinator.
The custom implementation does not depend on Foundry's built-in HTTP client or public Azure types.
It does depend on the Foundry C callback contract.

The interface covers catalog requests, registry resolution, blob enumeration, HEAD requests, and range downloads.
It does not cover telemetry, inference traffic, or execution-provider downloads.
In shared-host mode, model acquisition and its callbacks stay in the client process.

## Native interface

Use API version 4 or later. A runtime that does not support this version cannot register a custom transport.
Do not silently select an older API version when an application requests this feature.

Call `flConfigurationApi::SetModelHttpTransport` with a `flHttpTransport`.
C++ applications can use `Configuration::SetModelHttpTransport`.
Pass a null transport pointer to restore the default for future managers.
An existing manager keeps its original transport.

Set each HTTP structure's `version` to `FOUNDRY_LOCAL_HTTP_TRANSPORT_VERSION`.
This version is independent of the function-table API version.
All callbacks are required.

| Callback | Responsibility |
| --- | --- |
| `Retain` | Retain the application context. |
| `Release` | Release the application context. |
| `CreateOperation` | Create an operation without network access. |
| `Send` | Send one request and return its response headers. |
| `Read` | Read response bytes into a Foundry buffer. |
| `Cancel` | Interrupt the operation, including blocked network calls. |
| `ReleaseOperation` | Close the response and release operation resources. |

### Ownership

The configuration copies the callback table and retains the application context.
The application must keep its callback code available until the final context release.
The application can release its original ownership after registration.
Configuration replacement does not invalidate a manager's transport.

Foundry owns the request storage until `ReleaseOperation` returns.
The transport must not modify the request or any object reachable from it.
Response headers belong to the operation and must remain valid until its release.
Header arrays preserve duplicate fields.
The existing internal metadata and Azure response maps retain only one value per header name.
The callback contract preserves the array, but those internal boundaries do not provide general duplicate-header semantics.

`CreateOperation` must return promptly.
On success, it returns a non-null operation.
On failure, it returns a null operation.
Foundry releases each successful operation exactly once, including operations that never reach `Send`.

All callbacks use `FL_API_CALL`.
No exception, Rust panic, or managed exception can cross the C boundary.
Error statuses must come from the same native runtime's `Status_Create`.
Foundry takes ownership of each returned status.

### Streaming and cancellation

Foundry calls `Send` once per operation.
It then calls `Read` serially until EOF, failure, or cancellation.
Different operations can run concurrently.

`Read` receives a positive capacity and must return at most that many bytes.
A short read is valid.
A successful zero-byte read means EOF, not temporary unavailability.
The transport must not buffer an entire model file before the first read.

`Cancel` can run concurrently with `Send` or `Read`.
It must be thread-safe, idempotent, and prompt.
Cancellation is permanent for the operation, including cancellation before `Send`.
Blocked calls must return with `FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED`.
Foundry waits for active callbacks before operation release.

Do not protect `Cancel` with a mutex that a blocked `Send` or `Read` holds.
Do not call manager disposal or recursive model acquisition from a transport callback.

### HTTP rules

Version 1 supports GET, HEAD, and POST.
POST bodies contain exactly `body_size` bytes.
Header names are case-insensitive.
Header values are HTTP field bytes represented by NUL-terminated strings, not necessarily UTF-8.
Embedded NUL, CR, and LF bytes are not valid field content.

Preserve the URL path and query, including signed query parameters.
Preserve range, conditional, and checksum headers.
Return payload bytes after HTTP framing removal, without automatic content decompression.

The transport controls TLS trust, proxy selection, authentication, and connection pools.
It must not disable certificate checks as an example shortcut.
It must not automatically retry requests or follow redirects.
Return 3xx responses to Foundry's caller; this contract does not grant unrestricted redirect permission.

`timeout_ms` covers `Send` and all body reads, from entry into `Send`.
Zero means no deadline.
The transport must enforce the deadline during blocked calls.

HTTP errors are responses, not callback failures.
For example, `Send` returns success with status 403 for an HTTP 403 response.
Use `FOUNDRY_LOCAL_ERROR_NETWORK` for connection, TLS, timeout, and response-stream failures.
Do not include credentials, signed URLs, or raw request bodies in error messages.

Foundry retains its existing retry decisions, region selection, chunk concurrency, ETag checks, checksums, and resume state.
The custom transport does not write cache files or declare a model complete.
A custom transport failure never selects the built-in transport as a fallback.

The foundation buffers catalog and registry response bodies up to 16 MiB.
Blob responses use the streamed path and do not have this metadata limit.
Azure contexts have no cancellation callback registration.
The bridge therefore uses one lifetime-bound monitor per operation to interrupt blocked callbacks.

## Binding adapter designs

The native interface is the foundation.
The Rust, JavaScript, C#, and Python APIs below are proposed binding designs, not available SDK methods.
They show how each language can implement its own transport without using Foundry's default network stack.

Each adapter must also preserve the native runtime's lifetime.
It must not unload the library while callback state or returned error statuses still exist.

### Rust

Expose safe traits and hide the C function pointers:

```rust,ignore
pub trait ModelHttpTransport: Send + Sync + 'static {
    fn create_operation(
        &self,
        request: HttpRequest,
    ) -> Result<Box<dyn ModelHttpOperation>>;
}

pub trait ModelHttpOperation: Send + Sync + 'static {
    fn send(&self) -> Result<HttpResponseHead>;
    fn read(&self, buffer: &mut [u8]) -> Result<usize>;
    fn cancel(&self);
}

let config = FoundryLocalConfig::new("my-app")
    .model_http_transport(Arc::new(MyRustTransport::new(client)));
```

`HttpRequest` owns its method, URL, header vector, body bytes, and optional timeout.
Header values use byte vectors.
`HttpResponseHead` owns the status and duplicate-preserving header vector.
The bridge retains the response head until native operation release.

The bridge stores the transport in an `Arc`.
Native retain and release callbacks adjust ownership of an SDK wrapper around that `Arc`.
Each operation owns a boxed trait object.
Native operation release drops the box.
The bridge contains panics in fallible callbacks and returns a native error.
Infallible lifecycle and cancellation methods must not panic.

An asynchronous client adapter can use a supplied Tokio runtime:

```rust,ignore
let transport = ReqwestModelTransport::new(
    configured_client,
    runtime.handle().clone(),
);
```

The adapter schedules request and body futures on that runtime.
It blocks only native worker threads, not runtime worker threads.
It selects network futures against a cancellation token and the operation deadline.
The runtime must remain active until all operations end.

For `reqwest`, disable automatic redirects, request retries, and response decompression when the client supports them.
Check the chosen client version and features rather than assume its defaults satisfy the contract.
Preserve unused response-chunk bytes between `Read` calls.
Do not expose a `reqwest` type as a requirement of the Rust trait.

### JavaScript and TypeScript

Expose asynchronous operations, not synchronous callbacks on the JavaScript thread:

```typescript
// Proposed types, not an existing SDK export.
interface ModelHttpOperation {
  send(): Promise<{
    statusCode: number;
    headers: ReadonlyArray<readonly [string, Uint8Array]>;
  }>;
  read(capacity: number): Promise<Uint8Array>;
  cancel(): void;
  close(): void;
}

interface ModelHttpTransport {
  createOperation(request: OwnedHttpRequest): ModelHttpOperation;
}
```

`OwnedHttpRequest` contains the method, URL, headers, body, and timeout.
The addon copies native request data before it schedules JavaScript work.
It uses the C++ configuration wrapper to register the native table.
It must not bypass that wrapper with direct C ABI calls.

Use `Napi::ThreadSafeFunction` to schedule callbacks on the event loop.
Each native worker waits for the corresponding promise result.
The JavaScript event loop must remain free to complete that promise.
Reject synchronous SDK operations that need this transport; otherwise they can deadlock the event loop.

Use `node:http` or `node:https` for a baseline adapter.
These APIs expose raw header arrays and do not automatically follow redirects or decompress response bodies.
Use an `AbortController` or destroy the request to interrupt blocked operations.
Read through the stream's pull interface and preserve unused bytes between calls.

Do not use plain `fetch` without a compatibility review.
Automatic decompression and normalized headers can violate the native contract.
The bridge must cancel pending work during environment shutdown and wake each waiting native worker.

### C#

Expose a transport factory and an operation interface:

```csharp
// Proposed types, not existing SDK exports.
public interface IModelHttpTransport
{
    IModelHttpOperation CreateOperation(ModelHttpRequest request);
}

public interface IModelHttpOperation : IDisposable
{
    ModelHttpResponseHead Send();
    int Read(Span<byte> buffer);
    void Cancel();
}
```

Use `HttpClient.SendAsync` with `HttpCompletionOption.ResponseHeadersRead`.
Pass the operation token to both the request and stream reads.
Keep its `CancellationTokenSource` separate from any stream lock.
The deadline spans both request headers and subsequent reads.

Configure the HTTP handler with automatic redirects and decompression disabled.
Preserve repeated response headers without comma-based value splitting.

Root the callback delegates and transport state until native release.
Use `GCHandle` for application and operation handles.
Keep unmanaged response-header storage until operation release.
Catch managed exceptions in each fallible callback and translate them into native statuses.
Release handles only after native callbacks drain.

### Python

Expose a synchronous protocol that matches native worker calls:

```python
# Proposed protocols, not existing SDK exports.
class ModelHttpTransport(Protocol):
    def create_operation(self, request: HttpRequest) -> ModelHttpOperation: ...

class ModelHttpOperation(Protocol):
    def send(self) -> HttpResponseHead: ...
    def readinto(self, buffer: memoryview) -> int: ...
    def cancel(self) -> None: ...
    def close(self) -> None: ...
```

The cffi bridge keeps strong references to callbacks, context handles, and response-header buffers.
It exposes the native read buffer as a temporary writable view.
The application must not retain that view after the callback returns.
The bridge catches Python exceptions and creates native error statuses.

A socket-based adapter must release the GIL during blocked network calls.
Its cancellation path must interrupt the socket without the read lock.
Check the selected client's cancellation support; a cancellation flag alone cannot interrupt a blocked read.

An asynchronous adapter can schedule coroutines on an application-owned event loop.
Only native workers may wait for those coroutines.
The event loop must remain active until operation release.

## Examples and follow-up

The native example is in [`examples/custom_transport`](../examples/custom_transport).
It uses application-owned callbacks rather than an Azure transport implementation.

Production binding adapters belong in their respective SDKs.
Their follow-up work must cover callback lifetime, cancellation before and during network calls,
partial reads, duplicate headers, deadlines, runtime shutdown, and secret-safe errors.
