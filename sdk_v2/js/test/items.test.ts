// Item shape round-trip + factory tests.
//
// Doesn't need a model — only exercises Request.addItem / getItem and the
// `Item` factory helpers. Runs whenever the native addon is present.
import { describe, expect, it } from "vitest";

import { Item } from "../src/items.js";
import { Request } from "../src/request.js";

import { haveNativePrereqs, nativePrereqsDiagnostic } from "./_fixtures/cacheOnlyManager.js";

const describeIfBuilt = haveNativePrereqs ? describe : describe.skip;

if (!haveNativePrereqs) {
  console.warn(`[Item tests] SKIPPED — ${nativePrereqsDiagnostic}`);
}

describeIfBuilt("Item factory", () => {
  it("text() builds a default text item", () => {
    expect(Item.text("hello")).toEqual({ type: "text", text: "hello", textType: "default" });
  });

  it("text() honours textType", () => {
    expect(Item.text("think", "reasoning")).toEqual({
      type: "text",
      text: "think",
      textType: "reasoning",
    });
  });

  it("systemMessage builds a system role message with content", () => {
    expect(Item.systemMessage("you are helpful")).toEqual({
      type: "message",
      role: "system",
      content: "you are helpful",
    });
  });

  it("userMessage accepts string content", () => {
    expect(Item.userMessage("hi")).toEqual({ type: "message", role: "user", content: "hi" });
  });

  it("userMessage accepts parts array", () => {
    const parts = [Item.text("a"), Item.text("b")];
    expect(Item.userMessage(parts)).toEqual({ type: "message", role: "user", parts });
  });

  it("toolCall / toolResult build the right shape", () => {
    expect(Item.toolCall("id-1", "lookup", '{"x":1}')).toEqual({
      type: "toolCall",
      callId: "id-1",
      name: "lookup",
      arguments: '{"x":1}',
    });
    expect(Item.toolResult("id-1", "42")).toEqual({
      type: "toolResult",
      callId: "id-1",
      result: "42",
    });
  });

  it("imageFromUri / audioFromUri omit format when not supplied", () => {
    expect(Item.imageFromUri("file:///x.png")).toEqual({ type: "image", uri: "file:///x.png" });
    expect(Item.audioFromUri("file:///x.wav", "wav")).toEqual({
      type: "audio",
      uri: "file:///x.wav",
      format: "wav",
    });
  });
});

describeIfBuilt("Request round-trip through the native layer", () => {
  it("preserves a plain text item", () => {
    const req = new Request();
    req.addItem(Item.text("alpha"));
    expect(req.itemCount).toBe(1);
    const got = req.getItem(0);
    expect(got.type).toBe("text");
    if (got.type === "text") {
      expect(got.text).toBe("alpha");
      expect(got.textType).toBe("default");
    }
  });

  it("preserves a system message with string content", () => {
    const req = new Request();
    req.addItem(Item.systemMessage("be brief"));
    const got: Item = req.getItem(0);
    expect(got.type).toBe("message");
    if (got.type === "message") {
      expect(got.role).toBe("system");
      expect(got.content).toBe("be brief");
    }
  });

  it("preserves a user message with text parts", () => {
    const req = new Request();
    req.addItem(Item.userMessage([Item.text("one"), Item.text("two")]));
    const got: Item = req.getItem(0);
    expect(got.type).toBe("message");
    if (got.type === "message") {
      expect(got.role).toBe("user");
      // Single-text-part messages are flattened to `content` by the native side.
      // For multi-part inputs the parts array is preserved.
      expect(got.parts?.length ?? 0).toBeGreaterThanOrEqual(2);
    }
  });

  it("preserves a tool call item", () => {
    const req = new Request();
    req.addItem(Item.toolCall("c-1", "weather", '{"city":"Redmond"}'));
    const got: Item = req.getItem(0);
    expect(got.type).toBe("toolCall");
    if (got.type === "toolCall") {
      expect(got.callId).toBe("c-1");
      expect(got.name).toBe("weather");
      expect(got.arguments).toBe('{"city":"Redmond"}');
    }
  });

  it("setOptions accepts string, number, boolean values", () => {
    const req = new Request();
    req.addItem(Item.text("x"));
    // Just verifying the call doesn't throw with mixed types.
    expect(() =>
      req.setOptions({
        search: { temperature: 0.7, maxOutputTokens: 32, doSample: true },
        additionalOptions: { name: "test" },
      }),
    ).not.toThrow();
  });

  it("getItemCount and chained addItem produce a consistent count", () => {
    const req = new Request()
      .addItem(Item.systemMessage("s"))
      .addItem(Item.userMessage("u"))
      .addItem(Item.assistantMessage("a"));
    expect(req.itemCount).toBe(3);
  });

  it("cancel on an unattached request is a no-op", () => {
    const req = new Request();
    expect(() => req.cancel()).not.toThrow();
  });
});

describeIfBuilt("Raw-bytes Item inputs (zero-copy pinned)", () => {
  it("BytesItem round-trips an arbitrary byte payload", () => {
    const payload = new Uint8Array([0x00, 0x01, 0x7f, 0x80, 0xff, 0x42]);
    const req = new Request();
    req.addItem(Item.bytes(payload));
    const got = req.getItem(0);
    expect(got.type).toBe("bytes");
    if (got.type === "bytes") {
      expect(Array.from(got.data)).toEqual(Array.from(payload));
    }
  });

  it("BytesItem zero-copy: mutating source after addItem is visible to round-trip", () => {
    // Documents the no-copy contract — the native layer reads the source
    // Uint8Array directly. Mutation after addItem is *intentionally* visible.
    const payload = new Uint8Array([1, 2, 3, 4]);
    const req = new Request();
    req.addItem(Item.bytes(payload));
    payload[0] = 0xff;
    payload[3] = 0xff;
    const got = req.getItem(0);
    expect(got.type).toBe("bytes");
    if (got.type === "bytes") {
      expect(Array.from(got.data)).toEqual([0xff, 2, 3, 0xff]);
    }
  });

  it("BytesItem: same buffer can be added to multiple requests without copying", () => {
    const payload = new Uint8Array(64 * 1024);
    for (let i = 0; i < payload.length; ++i) {
      payload[i] = (i * 31) & 0xff;
    }

    const req1 = new Request();
    const req2 = new Request();
    req1.addItem(Item.bytes(payload));
    req2.addItem(Item.bytes(payload));

    const a = req1.getItem(0);
    const b = req2.getItem(0);
    expect(a.type).toBe("bytes");
    expect(b.type).toBe("bytes");
    if (a.type === "bytes" && b.type === "bytes") {
      expect(a.data.length).toBe(payload.length);
      expect(b.data.length).toBe(payload.length);
      expect(a.data[0]).toBe(payload[0]);
      expect(b.data[payload.length - 1]).toBe(payload[payload.length - 1]);
    }

    // Drop both requests; under GC pressure the pinned references must
    // release cleanly without use-after-free of the shared source.
    // (Without --expose-gc we can't force a collection — running this
    // sequence repeatedly is enough to catch a double-free or stale ref.)
    for (let i = 0; i < 8; ++i) {
      const r = new Request();
      r.addItem(Item.bytes(payload));
      void r.getItem(0);
    }
    (globalThis as { gc?: () => void }).gc?.();
    expect(payload[0]).toBe(0);
  });

  it("BytesItem: SharedArrayBuffer-backed view is rejected with TypeError", () => {
    const view = new Uint8Array(new SharedArrayBuffer(16));
    const req = new Request();
    expect(() => req.addItem(Item.bytes(view))).toThrow(/SharedArrayBuffer/);
  });

  it("BytesItem requires a data field", () => {
    const req = new Request();
    expect(() => req.addItem({ type: "bytes" } as never)).toThrow(/data/);
  });

  it("TensorItem round-trips float32 data with matching shape", () => {
    const floats = new Float32Array([1.5, -2.25, 0.0, 3.5]);
    const bytes = new Uint8Array(floats.buffer, floats.byteOffset, floats.byteLength);
    const req = new Request();
    req.addItem(Item.tensor("float", [2, 2], bytes));
    const got = req.getItem(0);
    expect(got.type).toBe("tensor");
    if (got.type === "tensor") {
      expect(got.dataType).toBe("float");
      expect(Array.from(got.shape)).toEqual([2, 2]);
      const roundtrip = new Float32Array(got.data.buffer, got.data.byteOffset, 4);
      expect(Array.from(roundtrip)).toEqual([1.5, -2.25, 0.0, 3.5]);
    }
  });

  it("TensorItem rejects a data size that doesn't match shape * elemSize", () => {
    const req = new Request();
    const wrong = new Uint8Array(7); // not a multiple of float32 elem size for shape [2,2]
    expect(() => req.addItem(Item.tensor("float", [2, 2], wrong))).toThrow(/expected/);
  });

  it("TensorItem rejects opaque data types on input", () => {
    const req = new Request();
    expect(() => req.addItem(Item.tensor("string", [1], new Uint8Array([0])))).toThrow(
      /unsupported|not supported|portable/,
    );
  });

  it("ImageItem accepts raw bytes with a format hint", () => {
    // Tiny fake PNG-ish payload; the native side doesn't decode here.
    const payload = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    const req = new Request();
    req.addItem(Item.imageFromData("png", payload));
    const got = req.getItem(0);
    expect(got.type).toBe("image");
    if (got.type === "image") {
      expect(got.format).toBe("png");
      expect(got.data && Array.from(got.data)).toEqual(Array.from(payload));
    }
  });

  it("ImageItem rejects both uri and data set", () => {
    const req = new Request();
    expect(() => req.addItem({ type: "image", uri: "file:///x.png", data: new Uint8Array([1]) } as never)).toThrow(
      /exactly one/,
    );
  });

  it("ImageItem rejects neither uri nor data", () => {
    const req = new Request();
    expect(() => req.addItem({ type: "image" } as never)).toThrow(/exactly one/);
  });

  it("ImageItem from data requires a format", () => {
    const req = new Request();
    expect(() => req.addItem({ type: "image", data: new Uint8Array([1, 2]) } as never)).toThrow(/format/);
  });

  it("AudioItem accepts raw bytes with format / sampleRate / channels", () => {
    const payload = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]);
    const req = new Request();
    req.addItem(Item.audioFromData("wav", payload, 16000, 1));
    const got = req.getItem(0);
    expect(got.type).toBe("audio");
    if (got.type === "audio") {
      expect(got.format).toBe("wav");
      expect(got.sampleRate).toBe(16000);
      expect(got.channels).toBe(1);
      expect(got.data && Array.from(got.data)).toEqual(Array.from(payload));
    }
  });
});
