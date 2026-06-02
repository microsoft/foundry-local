# C# Channel<T> with AllowSynchronousContinuations — ordering pitfall

When `Channel<T>` is created with `AllowSynchronousContinuations = true`, a call
to `channel.Writer.TryComplete()` may **synchronously** run any awaiting
consumer continuation (e.g. an `await foreach` exiting its loop and entering its
`finally`).

If the consumer's cleanup cancels a `CancellationTokenSource` that the producer
is also observing, code that runs in the producer **after** `TryComplete()`
will see `cts.IsCancellationRequested == true` even when the stream completed
naturally. This caused a real bug in `Session.ProcessStreamingRequestAsync`
where a successful drain was mis-attributed as a cancelled mid-stream and the
`Response` was disposed before the caller could observe it via
`StreamingResponse.FinalResponse`.

**Rule:** snapshot any state derived from the shared cts **before**
`TryComplete()`. Example (from `sdk_v2/cs/src/Session.cs`):

```csharp
responsePtr = _session.ProcessRequest(request.Ptr);
bool wasCancelledBeforeReturn = cts.IsCancellationRequested;  // snapshot
channel.Writer.TryComplete();                                  // may sync-run consumer cleanup
if (wasCancelledBeforeReturn) { /* cancelled path */ }
else                          { /* success path   */ }
```

The opposite ordering invariant is also useful: complete the channel **before**
publishing any "final" signal so consumers awaiting both observe iterator
completion strictly before the terminal value settles.
