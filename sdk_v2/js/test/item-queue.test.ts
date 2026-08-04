// ItemQueue native handle tests.
//
// Exercises the wrapper around `foundry_local::ItemQueue`. No real model
// needed — just push/pop/dispose round-trips and request-borrow semantics.
import { describe, expect, it } from "vitest";

import { ItemQueue } from "../src/item-queue.js";
import { Item } from "../src/items.js";
import { Request } from "../src/request.js";

import { haveNativePrereqs, nativePrereqsDiagnostic } from "./_fixtures/cacheOnlyManager.js";

const describeIfBuilt = haveNativePrereqs ? describe : describe.skip;

if (!haveNativePrereqs) {
  console.warn(`[ItemQueue tests] SKIPPED — ${nativePrereqsDiagnostic}`);
}

describeIfBuilt("ItemQueue", () => {
  it("construct + dispose round-trip; dispose is idempotent", () => {
    const q = new ItemQueue();
    expect(q.size).toBe(0);
    expect(q.finished).toBe(false);
    q.dispose();
    // Second dispose is a no-op (idempotent).
    expect(() => {
      q.dispose();
    }).not.toThrow();
    // Post-dispose access throws TypeError.
    expect(() => q.size).toThrow(/disposed/);
  });

  it("push / tryPop / size / finished round-trip", () => {
    const q = new ItemQueue();
    q.push(Item.text("alpha"));
    q.push(Item.userMessage("hi"));
    q.push(Item.bytes(new Uint8Array([1, 2, 3])));
    expect(q.size).toBe(3);

    const a = q.tryPop();
    expect(a?.type).toBe("text");
    if (a?.type === "text") {
      expect(a.text).toBe("alpha");
    }

    const b = q.tryPop();
    expect(b?.type).toBe("message");
    if (b?.type === "message") {
      expect(b.role).toBe("user");
      expect(b.content).toBe("hi");
    }

    const c = q.tryPop();
    expect(c?.type).toBe("bytes");
    if (c?.type === "bytes") {
      expect(Array.from(c.data)).toEqual([1, 2, 3]);
    }

    expect(q.tryPop()).toBeNull();
    expect(q.size).toBe(0);

    expect(q.finished).toBe(false);
    q.markFinished();
    expect(q.finished).toBe(true);

    q.dispose();
  });

  it("addItem(queue) borrows: the queue remains usable afterward", () => {
    const q = new ItemQueue();
    q.push(Item.text("one"));

    const req = new Request();
    req.addItem(q);
    expect(req.itemCount).toBe(1);

    // Borrow, not transfer — the JS side can keep pushing.
    q.push(Item.text("two"));
    expect(q.size).toBe(2);

    q.dispose();
    // GC of the request must not crash even though the queue handle has
    // been dropped — the request held only a non-owning borrow.
  });

  it("same queue can be added to two requests (multi-borrow, no double-free)", () => {
    const q = new ItemQueue();
    q.push(Item.text("shared"));

    const req1 = new Request();
    const req2 = new Request();
    req1.addItem(q);
    req2.addItem(q);
    expect(req1.itemCount).toBe(1);
    expect(req2.itemCount).toBe(1);

    // Drain via the queue itself — proves both requests just borrowed.
    expect(q.tryPop()?.type).toBe("text");
    expect(q.tryPop()).toBeNull();

    q.dispose();
  });

  it("zero-copy pinning survives push: mutation of the source is visible on pop", () => {
    const buf = new Uint8Array([10, 20, 30, 40]);
    const q = new ItemQueue();
    q.push(Item.bytes(buf));
    buf[0] = 0xff;
    buf[3] = 0xee;

    const popped = q.tryPop();
    expect(popped?.type).toBe("bytes");
    if (popped?.type === "bytes") {
      expect(Array.from(popped.data)).toEqual([0xff, 20, 30, 0xee]);
    }

    q.dispose();
  });

  it("addItem on a disposed queue throws TypeError matching /disposed/", () => {
    const q = new ItemQueue();
    q.dispose();
    const req = new Request();
    expect(() => req.addItem(q)).toThrow(/disposed/);
  });
});
