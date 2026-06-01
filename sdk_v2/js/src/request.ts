// Public `Request` class. Stateful builder over the native `foundry_local::Request`. Mirrors the C# `Request` in
// `sdk_v2/cs/src/Request.cs` and the Python `Request` in `sdk_v2/python/src/foundry_local_sdk/request.py`.
import { type NativeRequest, getAddon } from "./detail/native.js";
import { ItemQueue, unwrapNativeItemQueue } from "./item-queue.js";
import type { Item } from "./items.js";

const nativeByRequest = new WeakMap<Request, NativeRequest>();

/** Sampling / decoding parameters. Mirrors the C++ `foundry_local::SearchOptions`. */
export interface SearchOptions {
  temperature?: number;
  topP?: number;
  topK?: number;
  maxOutputTokens?: number;
  frequencyPenalty?: number;
  presencePenalty?: number;
  seed?: number;
  earlyStopping?: boolean;
  doSample?: boolean;
}

/** Tool-choice mode for tool-enabled requests. */
export type RequestToolChoice = "auto" | "none" | "required";

/** Per-request options passed via {@link Request.setOptions}. */
export interface RequestOptions {
  search?: SearchOptions;
  toolChoice?: RequestToolChoice;
  /** Additional provider-specific options forwarded as-is. */
  additionalOptions?: Readonly<Record<string, string | number | boolean | undefined>>;
}

export class Request {
  readonly #native: NativeRequest;

  constructor() {
    const addon = getAddon();
    this.#native = new addon.Request();
    nativeByRequest.set(this, this.#native);
  }

  /**
   * Append an input item. Pass an `ItemQueue` to wire a streaming producer;
   * the request *borrows* the queue (the JS side keeps it alive and the
   * caller is responsible for `dispose()`-ing it).
   */
  addItem(item: Item | ItemQueue): this {
    if (item instanceof ItemQueue) {
      // Forward the raw NativeItemQueue handle so the addon's InstanceOf
      // check fires and routes through the borrow path (take_ownership=false).
      this.#native.addItem(unwrapNativeItemQueue(item));
    } else {
      rejectSharedArrayBuffer(item);
      this.#native.addItem(item);
    }
    return this;
  }

  /** Apply per-request sampling/decoding options. */
  setOptions(options: RequestOptions): this {
    this.#native.setOptions(options);
    return this;
  }

  /** Number of items in the request. */
  get itemCount(): number {
    return this.#native.getItemCount();
  }

  /** Retrieve the item at `index`. Useful for round-trip tests. */
  getItem(index: number): Item {
    return this.#native.getItem(index) as Item;
  }

  /**
   * Cancel an in-flight request. Safe to call at any time — if the request
   * is not currently being processed by a session, this is a no-op.
   * Cancellation makes the matching `Session.processRequest()` reject with a
   * `FoundryLocalError` whose `code === FlErrorCode.OperationCancelled`.
   */
  cancel(): void {
    this.#native.cancel();
  }
}

/** @internal — used by `Session.processRequest()` to forward the underlying handle. */
export function unwrapNativeRequest(request: Request): NativeRequest {
  const n = nativeByRequest.get(request);
  if (n === undefined) {
    throw new TypeError("Session.processRequest: argument is not a valid Request");
  }
  return n;
}

// Raw-bytes Item inputs are pinned zero-copy by the native addon. We only
// support standard ArrayBuffer-backed views — SharedArrayBuffer has different
// threading and detach semantics that the addon's pin path doesn't model. The
// check lives here (not native) because the JS-side `instanceof` is the
// cleanest discriminator; N-API has no direct IsSharedArrayBuffer predicate.
function rejectSharedArrayBuffer(item: Item): void {
  if (item.type !== "bytes" && item.type !== "tensor" && item.type !== "image" && item.type !== "audio") {
    return;
  }
  const data = (item as { data?: unknown }).data;
  if (data === undefined || data === null) {
    return;
  }
  if (
    typeof SharedArrayBuffer !== "undefined" &&
    ArrayBuffer.isView(data) &&
    data.buffer instanceof SharedArrayBuffer
  ) {
    throw new TypeError(`${item.type}: SharedArrayBuffer-backed views are not supported`);
  }
}
