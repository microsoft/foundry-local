# Python streaming response shape (sdk_v2/python)

`Session.process_streaming_request(request)` returns a `StreamingResponse`,
not an `Iterator[Item]`. Mirrors the JS SDK's `StreamingResponse extends
AsyncIterable<Item>` with `final_response` (parallel work adds the same
shape to C#).

## Usage

```python
session.set_streaming(True)
with session.process_streaming_request(req) as stream:
    for item in stream:
        ...  # incremental
    with stream.final_response as final:
        print(final.finish_reason, final.get_usage())
```

## Key invariants

- Iterable at most once. Second `iter()` raises `FoundryLocalException`.
- `final_response` raises if accessed before iteration completes.
- On cancellation (caller broke iter), `final_response` raises
  `"Stream was cancelled."` — the native Response on cancel is undefined.
- On worker exception, both `__iter__` and `final_response` re-raise.
- `__exit__` is idempotent: cancels in-flight request, drains queue,
  joins worker, closes the un-consumed final_response, releases the
  session's `_streaming_in_flight` lock.
- The session lock is released either when the iterator terminates (its
  `finally`) or in `__exit__` — whichever fires first. Use the
  `_lock_released` flag to keep both paths idempotent.

## No `on_final_response` callback

The previous callback-based API was removed (hard cut, no shim). It ran
on the worker thread and had implicit lifetime — the new wrapper's
return-value shape fixes both.
