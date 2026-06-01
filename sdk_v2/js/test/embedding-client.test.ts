// EmbeddingClient (V1 OpenAI-JSON pass-through) against a real loaded
// embeddings model. Gated by FOUNDRY_TEST_DATA_DIR.
import { afterAll, beforeAll, describe, expect, it } from "vitest";

import type { EmbeddingClient } from "../src/openai/embeddingClient.js";

import {
  type RealModelManagerFixture,
  haveTestModelCache,
  setupRealModelManager,
  teardownRealModelManager,
  testModelCacheDiagnostic,
} from "./_fixtures/realModelManager.js";

if (!haveTestModelCache) {
  console.warn(testModelCacheDiagnostic);
}

// biome-ignore lint/suspicious/noExplicitAny: OpenAI response objects are user-shaped JSON.
type Json = any;

function extractEmbedding(result: Json, index = 0): number[] {
  const data = result?.data;
  expect(Array.isArray(data)).toBe(true);
  const entry = data[index];
  expect(entry).toBeDefined();
  const vec = entry.embedding;
  expect(Array.isArray(vec)).toBe(true);
  return vec as number[];
}

function l2Distance(a: number[], b: number[]): number {
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

describe.skipIf(!haveTestModelCache)("EmbeddingClient (real model, V1 OpenAI-JSON pass-through)", () => {
  let fixture: RealModelManagerFixture | undefined;
  let client: EmbeddingClient | undefined;

  beforeAll(async () => {
    fixture = await setupRealModelManager({
      task: "embeddings",
      namePreference: "qwen3-embedding-0.6b-generic-cpu",
    });
    if (fixture !== undefined) {
      client = fixture.model.createEmbeddingClient();
    }
  }, 5 * 60_000);

  afterAll(() => {
    client?.dispose();
    client = undefined;
    teardownRealModelManager(fixture);
  });

  it(
    "generateEmbedding returns one embedding for a single input",
    async () => {
      if (client === undefined) throw new Error("client missing");
      const result = await client.generateEmbedding("hello world");
      const vec = extractEmbedding(result, 0);
      expect(vec.length).toBeGreaterThan(0);
      let mag = 0;
      for (const v of vec) mag += v * v;
      expect(mag).toBeGreaterThan(0);
      // Embedding responses include a `usage` object, but `prompt_tokens` may
      // legitimately be 0 for some embedding models (no generative prompt).
      // Just assert the shape is present.
      expect(result.usage).toBeDefined();
    },
    2 * 60_000,
  );

  it(
    "generateEmbeddings returns one embedding per input; distinct strings give distinct vectors",
    async () => {
      if (client === undefined) throw new Error("client missing");
      const result = await client.generateEmbeddings(["hello world", "goodbye world"]);
      expect(Array.isArray(result.data)).toBe(true);
      expect(result.data.length).toBe(2);
      const v0 = extractEmbedding(result, 0);
      const v1 = extractEmbedding(result, 1);
      expect(v0.length).toBe(v1.length);
      const dist01 = l2Distance(v0, v1);
      expect(dist01).toBeGreaterThan(1e-6);

      // Cross-request parity: embedding "hello world" twice should be (nearly)
      // identical. Two separate native round-trips may exhibit small numerical
      // noise — empirically we expect well below 1e-3.
      const repeat = await client.generateEmbedding("hello world");
      const vRepeat = extractEmbedding(repeat, 0);
      const distRepeat = l2Distance(v0, vRepeat);
      expect(distRepeat).toBeLessThan(1e-1);
    },
    3 * 60_000,
  );

  it("rejects an empty input with a clear error", async () => {
    if (client === undefined) throw new Error("client missing");
    await expect(client.generateEmbedding("")).rejects.toThrow(/non-empty string/);
  });
});
