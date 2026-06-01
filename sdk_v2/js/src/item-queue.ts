// Public `ItemQueue` class. Wraps the one Item subtype that genuinely holds a native handle on the JS side —
// push/pop/finished state has to be addressable by both the JS producer and the native consumer thread, so it
// can't be a plain object on the `Item` discriminated union.
import { type NativeItemQueue, getAddon } from "./detail/native.js";
import type { Item } from "./items.js";

const nativeByQueue = new WeakMap<ItemQueue, NativeItemQueue>();

/**
 * A queue of items used for streaming input or output.
 *
 * Mirrors the C++ `foundry_local::ItemQueue` and Python `ItemQueue`. Add a
 * queue to a `Request` with `request.addItem(queue)`; the request *borrows*
 * the queue (it does NOT take ownership), so the JS caller is responsible
 * for `dispose()`-ing the queue when done. The producer keeps pushing items
 * and eventually calls `markFinished()`; the session consumes from the queue
 * on a native thread.
 *
 * Always dispose. Prefer `using` syntax:
 *
 * ```ts
 * using q = new ItemQueue();
 * request.addItem(q);
 * for (const chunk of chunks) q.push(Item.bytes(chunk));
 * q.markFinished();
 * ```
 */
export class ItemQueue implements Disposable {
  readonly #native: NativeItemQueue;
  #disposed = false;

  constructor() {
    const addon = getAddon();
    this.#native = new addon.ItemQueue();
    nativeByQueue.set(this, this.#native);
  }

  /**
   * Transfer ownership of `item` into the queue. Raw-bytes Items keep their
   * existing zero-copy pinning contract — the underlying Uint8Array stays
   * pinned for the lifetime of the queue rather than a request.
   */
  push(item: Item): void {
    this.#checkDisposed();
    this.#native.push(item);
  }

  /** Pop the front item, or return `null` if the queue is empty. */
  tryPop(): Item | null {
    this.#checkDisposed();
    const popped = this.#native.tryPop();
    return popped === null ? null : (popped as Item);
  }

  /** Number of items currently in the queue. */
  get size(): number {
    this.#checkDisposed();
    return this.#native.size();
  }

  /** Mark the queue as finished. The consumer will stop after draining it. */
  markFinished(): void {
    this.#checkDisposed();
    this.#native.markFinished();
  }

  /** Whether `markFinished()` has been called. */
  get finished(): boolean {
    this.#checkDisposed();
    return this.#native.finished();
  }

  /**
   * Release the native handle. Idempotent — a second call is a no-op. After
   * disposal every other method/getter throws `TypeError`.
   */
  dispose(): void {
    if (this.#disposed) return;
    this.#disposed = true;
    this.#native.dispose();
  }

  [Symbol.dispose](): void {
    this.dispose();
  }

  #checkDisposed(): void {
    if (this.#disposed) {
      throw new TypeError("ItemQueue: already disposed");
    }
  }
}

/** @internal — exposed for `Request.addItem` so the native side can branch. */
export function unwrapNativeItemQueue(queue: ItemQueue): NativeItemQueue {
  const n = nativeByQueue.get(queue);
  if (n === undefined) {
    throw new TypeError("ItemQueue: not a valid ItemQueue");
  }
  return n;
}
