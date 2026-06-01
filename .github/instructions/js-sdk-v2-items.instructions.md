---
description: Use when working with the JS v2 SDK Item discriminated union — writing item factories, walking streamed items, or porting C++ Item code to JS.
applyTo: sdk_v2/js/**
---

# JS v2 SDK — Item shape conventions

## Discriminator is `type` (consistent with all other layers)

The JS surface uses `type` as the discriminator string on the `Item` discriminated union
(see `sdk_v2/js/src/items.ts`). This matches the naming convention used everywhere else in the SDK:

- Internal C++: `fl::Item.type`
- C ABI: `flItemType` enum, `flItemApi.GetType`
- C++ wrapper: `foundry_local::Item::GetType()`

No translation between layers — same name, same concept.

```ts
for (const item of items) {
  if (item.type === "text") { ... }
  if (item.type === "message") { ... }
}
```

## MessageItem content shape

A `MessageItem` exposes content in one of two mutually-exclusive forms:

- **Single default text part** → `content: string` (flat string for the common case).
- **Multi-part content** → `parts: Item[]` (array of child items, typically text + image/audio/tool-result mix).

When constructing a `MessageItem` you may pass either shape; when reading one that came back from native,
check `parts` first, then fall back to `content`.

## Native field names mirror the C++ wrapper

ToolCallItem fields are `callId` and `arguments` (not `id` / `argumentsJson` / `toolCallId`). The JS surface
deliberately mirrors the C++ wrapper layer to avoid a translation shim that would drift over time. If you need
different naming on the public TS surface, add it as a derived accessor — do not rename in the addon layer.

## Items are plain JS objects + a value-namespace sharing the type name

The `Item` identifier is **both** the discriminated-union type and the value-namespace of factory helpers.
TS allows a type and a `const` to share a name (they live in separate namespaces):

```ts
export type Item = TextItem | MessageItem | BytesItem | ... ;
export const Item = Object.freeze({ text, message, bytes, tensor, ... });
```

Consumers import the single identifier and get both:

```ts
import { Item } from "foundry-local-sdk";
function fn(x: Item) { ... }            // uses the type
req.addItem(Item.userMessage("hi"));    // uses the value
```

Do **not** add classes for plain-data items. Classes are reserved for things-with-lifetimes
(`Manager`, `Model`, sessions, `ItemQueue`).

## Raw-bytes items use zero-copy pinned references — NOT a deep copy

`BytesItem`, `TensorItem`, and the from-data variants of `ImageItem` / `AudioItem` reach native via
`Request.addItem`. The native layer **does not copy** the source `Uint8Array` / `Buffer`. Instead it:

1. Pins the source via an N-API reference (so V8 GC can't reclaim it).
2. Hands the raw data pointer + size to the C++ wrapper's owning-deleter constructor
   (`Item::Bytes(data, size, deleter)` and friends).
3. The deleter releases the N-API reference when the owning `Request` (or `ItemQueue`) is destroyed.

**Public lifetime contract** (documented on each factory's JSDoc):

> Source buffers must remain unmodified until the `Request` that received the item is dropped.
> Mutating the source after `addItem` is observed by the running inference and produces undefined results.

Same buffer added to N requests = N pins on the same backing store. The bytes are never duplicated. This is
the entire reason the JS SDK does **not** expose a `transferOwnership` flag on `Request.addItem` — the
plain-object Item + buffer-pinning combo gives multi-request reuse for free, with no chance of double-free.

Implementation details for the pinning machinery:

- One `Napi::ThreadSafeFunction` is created at addon `Init` and stored in `AddonData::buffer_release_tsfn`.
  Every pinned-buffer deleter `Acquire`s the TSFN on item creation and `Release`s it from the deleter. The
  addon holds its own initial reference, released by `~AddonData` at env teardown.
- The TSFN bounce is required because deleters may fire on a non-JS thread (e.g. the ItemQueue consumer
  thread) — `napi_delete_reference` is only safe to call on the JS thread.
- `SharedArrayBuffer`-backed views are rejected at the JS-side guard in `Request.addItem` (one `instanceof`
  against `env.Global().SharedArrayBuffer`). The native code does not need to know about this — keeping the
  policy on the SDK boundary keeps the addon free of dispatch noise.
- Zero-length buffers skip the pin entirely (no-op deleter, nullptr data).

If you find yourself wanting to add a copy path "for safety" or a size threshold to opt into copying,
**don't**. The single-path design was an explicit architectural call — diagnosability of "did this Item copy?"
beats the marginal savings on tiny payloads.

## `ItemQueue` is the only Item that is a class, not a plain object

`ItemQueue` holds a real native handle because push/pop/markFinished/isFinished are stateful operations shared
with a consuming session. It is deliberately **not** part of the `Item` plain-object discriminated union —
`Request.addItem` takes `Item | ItemQueue`, with the union widening at the addItem boundary.

`Request.addItem(queue)` calls the C++ wrapper with `take_ownership=false`: JS keeps the queue alive via its
TS wrapper; the request just borrows the handle for the duration of inference. The same queue can be added to
N requests with no copy and no double-free. There is **no `transferOwnership` flag on the public API** — the
class identity does the dispatch.

The class-shaped Item wiring is composition-over-inheritance and has two layers, because the TS class wraps
the native ObjectWrap rather than being it:

```
TS ItemQueue (src/item-queue.ts)
  ├─ holds:    NativeItemQueue handle (via WeakMap)
  ├─ implements: Disposable, Symbol.dispose
  └─ on addItem: unwrapNativeItemQueue(this) → native handle
                                                ↓
NativeItemQueue (native/src/item_queue.cc)  — Napi::ObjectWrap
  └─ holds: std::unique_ptr<foundry_local::ItemQueue>
                                                ↓
addon-side Request::AddItem checks InstanceOf(addon_data->item_queue_ctor):
  matched → impl_->AddItem(*wrapper->impl(), take_ownership=false)
  unmatched → JsToItem(plain object) → impl_->AddItem(std::move(item))
```

The `InstanceOf` check fires against the **native** handle, not the public TS class. So `Request.addItem` in
TS must unwrap the public class to its native handle before calling into native. This is the pattern any
future class-shaped Item will need to follow.

Disposed queues throw `TypeError(/disposed/)` at the JS boundary on every method (including a re-add attempt
from `Request.addItem`).

`tryPop` returns plain-object Items only. Nested queues (the C ABI allows them) are deliberately unsupported
on the JS surface and `tryPop` throws if it encounters one — AudioSession's live-PCM path does not need
recursion, and any future need can revisit.
