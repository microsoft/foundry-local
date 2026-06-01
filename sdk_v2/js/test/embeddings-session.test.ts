// EmbeddingsSession tests against a real loaded embeddings model.
// Gated by FOUNDRY_TEST_DATA_DIR.
import { afterAll, afterEach, beforeAll, beforeEach, describe, expect, it } from "vitest";

import { Item, type TensorItem } from "../src/items.js";
import { Request } from "../src/request.js";
import { EmbeddingsSession } from "../src/session.js";

import {
  type RealModelManagerFixture,
  haveTestModelCache,
  setupRealModelManager,
  teardownRealModelManager,
  testModelCacheDiagnostic,
} from "./_fixtures/realModelManager.js";

import {
  type CacheOnlyManagerFixture,
  haveNativePrereqs,
  setupCacheOnlyManager,
  teardownCacheOnlyManager,
} from "./_fixtures/cacheOnlyManager.js";

if (!haveTestModelCache) {
  console.warn(testModelCacheDiagnostic);
}

// Reinterpret a TensorItem's raw float bytes as a Float32Array.
function tensorToFloats(item: TensorItem): Float32Array {
  expect(item.dataType).toBe("float");
  // The underlying buffer may be a slice; copy the relevant view into a
  // fresh, properly-aligned Float32Array.
  const view = new Uint8Array(item.data);
  const aligned = new Uint8Array(view.byteLength);
  aligned.set(view);
  return new Float32Array(aligned.buffer);
}

function l2Distance(a: Float32Array, b: Float32Array): number {
  expect(a.length).toBe(b.length);
  let acc = 0;
  for (let i = 0; i < a.length; ++i) {
    const da = a[i] as number;
    const db = b[i] as number;
    const d = da - db;
    acc += d * d;
  }
  return Math.sqrt(acc);
}

function isTensor(item: Item): item is TensorItem {
  return item.type === "tensor";
}

describe.skipIf(!haveTestModelCache)("EmbeddingsSession (real model)", () => {
  let fixture: RealModelManagerFixture | undefined;
  let session: EmbeddingsSession | undefined;

  beforeAll(async () => {
    fixture = await setupRealModelManager({
      task: "embeddings",
      namePreference: "qwen3-embedding-0.6b-generic-cpu",
    });
  }, 5 * 60_000);

  afterAll(() => {
    teardownRealModelManager(fixture);
  });

  beforeEach(() => {
    if (fixture === undefined) throw new Error("fixture missing");
    session = new EmbeddingsSession(fixture.model);
  });

  afterEach(() => {
    session?.dispose();
    session = undefined;
  });

  it(
    "processRequest with a single text input produces one non-zero embedding tensor",
    async () => {
      if (session === undefined) throw new Error("session missing");
      const req = new Request().addItem(Item.text("hello world"));
      const resp = await session.processRequest(req);

      expect(resp.output.length).toBe(1);
      const out = resp.output[0] as Item;
      expect(isTensor(out)).toBe(true);
      const tensor = out as TensorItem;
      expect(tensor.shape.length).toBeGreaterThan(0);
      expect(tensor.data.length).toBeGreaterThan(0);

      const v = tensorToFloats(tensor);
      // Embedding must not be the zero vector.
      let mag = 0;
      for (const f of v) {
        mag += f * f;
      }
      expect(mag).toBeGreaterThan(0);
    },
    2 * 60_000,
  );

  it(
    "processRequest with a batch of inputs returns one tensor per input with distinct vectors",
    async () => {
      if (session === undefined) throw new Error("session missing");
      const inputs = ["the quick brown fox", "machine learning is fun", "Paris is the capital of France"];
      const req = new Request();
      for (const s of inputs) {
        req.addItem(Item.text(s));
      }
      const resp = await session.processRequest(req);

      expect(resp.output.length).toBe(inputs.length);
      const tensors: Float32Array[] = [];
      for (const item of resp.output) {
        expect(isTensor(item as Item)).toBe(true);
        tensors.push(tensorToFloats(item as TensorItem));
      }
      // All vectors must have the same length and be pairwise distinct.
      const len = tensors[0]?.length ?? 0;
      expect(len).toBeGreaterThan(0);
      for (const t of tensors) {
        expect(t.length).toBe(len);
      }
      for (let i = 0; i < tensors.length; ++i) {
        for (let j = i + 1; j < tensors.length; ++j) {
          const a = tensors[i] as Float32Array;
          const b = tensors[j] as Float32Array;
          expect(l2Distance(a, b)).toBeGreaterThan(1e-3);
        }
      }
    },
    2 * 60_000,
  );

  it(
    "embedding the same input twice produces (near-)identical vectors",
    async () => {
      if (session === undefined) throw new Error("session missing");
      const input = "deterministic embeddings test";
      const r1 = await session.processRequest(new Request().addItem(Item.text(input)));
      const r2 = await session.processRequest(new Request().addItem(Item.text(input)));
      const v1 = tensorToFloats(r1.output[0] as TensorItem);
      const v2 = tensorToFloats(r2.output[0] as TensorItem);
      expect(l2Distance(v1, v2)).toBeLessThan(1e-3);
    },
    2 * 60_000,
  );

  it("dispose() flips the disposed flag and is idempotent", () => {
    if (fixture === undefined) throw new Error("fixture missing");
    const oneShot = new EmbeddingsSession(fixture.model);
    expect(oneShot.disposed).toBe(false);
    oneShot.dispose();
    expect(oneShot.disposed).toBe(true);
    expect(() => oneShot.dispose()).not.toThrow();
  });
});

describe("EmbeddingsSession constructor type guard", () => {
  it("throws TypeError when constructed with a non-Model argument", () => {
    expect(() => new EmbeddingsSession({} as never)).toThrow(TypeError);
    expect(() => new EmbeddingsSession({} as never)).toThrow(/Model/);
  });
});

// Wrong-task validation lives in JS and runs BEFORE the native ctor, so it
// fires whether or not the model has been loaded. We use the cache-only
// fixture (which has chat-completion catalog entries) and grab an UNLOADED
// chat model — far cheaper than loading a second real model.
const describeWrongTask = haveNativePrereqs ? describe : describe.skip;

describeWrongTask("EmbeddingsSession wrong-task validation", () => {
  let fixture: CacheOnlyManagerFixture | undefined;

  beforeAll(() => {
    fixture = setupCacheOnlyManager({ appName: "foundry-local-js-sdk-v2-embeddings-wrong-task" });
  });

  afterAll(() => {
    if (fixture !== undefined) {
      teardownCacheOnlyManager(fixture);
    }
  });

  it("throws TypeError when constructed with a non-embeddings model", async () => {
    if (fixture === undefined) throw new Error("fixture missing");
    const chatModel = await fixture.manager.catalog.getModel("phi-4-mini-instruct");
    const task = chatModel.info.task ?? "(unset)";
    expect(task).toBe("chat-completion");
    expect(() => new EmbeddingsSession(chatModel)).toThrow(TypeError);
    expect(() => new EmbeddingsSession(chatModel)).toThrow(/embeddings/);
    expect(() => new EmbeddingsSession(chatModel)).toThrow(new RegExp(task));
  });
});
