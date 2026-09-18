// Model lifecycle tests: load / unload / re-load against a real cached model.
// Gated by FOUNDRY_TEST_DATA_DIR.
import { afterAll, beforeAll, describe, expect, it } from "vitest";

import { isFoundryLocalError } from "../src/detail/errors.js";

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

describe.skipIf(!haveTestModelCache)("Model lifecycle (real model)", () => {
  let fixture: RealModelManagerFixture | undefined;

  beforeAll(async () => {
    fixture = await setupRealModelManager();
  }, 5 * 60_000);

  afterAll(() => {
    teardownRealModelManager(fixture);
  });

  it("is loaded after setup", async () => {
    expect(await fixture?.model.isLoaded()).toBe(true);
  });

  it(
    "unload() / load() round-trips cleanly",
    async () => {
      const m = fixture?.model;
      if (m === undefined) throw new Error("fixture missing");
      await m.unload();
      expect(await m.isLoaded()).toBe(false);
      await m.load();
      expect(await m.isLoaded()).toBe(true);
    },
    2 * 60_000,
  );

  it(
    "download() on an already-cached model resolves without error",
    async () => {
      const m = fixture?.model;
      if (m === undefined) throw new Error("fixture missing");
      // Already cached (we just loaded it); download should short-circuit.
      await expect(m.download()).resolves.toBeUndefined();
    },
    2 * 60_000,
  );

  it("calling load() on an already-loaded model is idempotent (or surfaces a clear error)", async () => {
    const m = fixture?.model;
    if (m === undefined) throw new Error("fixture missing");
    try {
      await m.load();
      expect(await m.isLoaded()).toBe(true);
    } catch (e) {
      // If the native side rejects re-loading, the rejection must be a
      // tagged FoundryLocalError — never a generic Error.
      expect(isFoundryLocalError(e)).toBe(true);
    }
  });
});
