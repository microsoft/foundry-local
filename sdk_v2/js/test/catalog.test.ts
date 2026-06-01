// Catalog tests for the v2 JS SDK. Uses the shared cache-only fixture
// helper so this file constructs exactly one Manager + cache directory.
import { afterAll, beforeAll, describe, expect, it } from "vitest";

import type { Catalog } from "../src/catalog.js";
import { Model } from "../src/model.js";

import {
  type CacheOnlyManagerFixture,
  haveNativePrereqs,
  nativePrereqsDiagnostic,
  setupCacheOnlyManager,
  teardownCacheOnlyManager,
} from "./_fixtures/cacheOnlyManager.js";

const describeIfBuilt = haveNativePrereqs ? describe : describe.skip;

if (!haveNativePrereqs) {
  // eslint-disable-next-line no-console
  console.warn(`[Catalog tests] SKIPPED — ${nativePrereqsDiagnostic}`);
}

describeIfBuilt("Catalog (cache-only)", () => {
  let fixture: CacheOnlyManagerFixture;
  let catalog: Catalog;

  beforeAll(() => {
    fixture = setupCacheOnlyManager({ appName: "foundry-local-js-sdk-v2-catalog-tests" });
    catalog = fixture.manager.catalog;
  });

  afterAll(() => {
    teardownCacheOnlyManager(fixture);
  });

  it("name returns a non-empty string", () => {
    expect(typeof catalog.name).toBe("string");
    expect(catalog.name.length).toBeGreaterThan(0);
  });

  it("getModels returns both fixture models", async () => {
    const models = await catalog.getModels();
    expect(models).toHaveLength(2);
    for (const m of models) {
      expect(m).toBeInstanceOf(Model);
    }
    const aliases = models.map((m) => m.info.alias).sort();
    expect(aliases).toEqual(["phi-4-mini-instruct", "qwen2.5-0.5b-instruct"]);
  });

  it("getModels returned ids match the fixture", async () => {
    const models = await catalog.getModels();
    const ids = models.map((m) => m.info.id).sort();
    expect(ids).toEqual(["phi-4-mini-instruct-generic-cpu:2", "qwen2.5-0.5b-instruct-generic-cpu:1"]);
  });

  it("getModel resolves a known alias to a Model with matching info", async () => {
    const model = await catalog.getModel("phi-4-mini-instruct");
    expect(model).toBeInstanceOf(Model);
    const info = model.info;
    expect(info.alias).toBe("phi-4-mini-instruct");
    expect(info.id).toBe("phi-4-mini-instruct-generic-cpu:2");
    expect(info.name).toBe("phi-4-mini-instruct-generic-cpu");
    expect(info.version).toBe(2);
    expect(info.task).toBe("chat-completion");
    expect(info.publisher).toBe("Microsoft");
  });

  it("getModel resolves a different known alias", async () => {
    const model = await catalog.getModel("qwen2.5-0.5b-instruct");
    expect(model).toBeInstanceOf(Model);
    expect(model.info.id).toBe("qwen2.5-0.5b-instruct-generic-cpu:1");
  });

  it("getModel rejects with a descriptive error for an unknown alias", async () => {
    await expect(catalog.getModel("does-not-exist-anywhere")).rejects.toThrow(
      /does-not-exist-anywhere/,
    );
  });

  it("getModelVariant resolves a full model id", async () => {
    const variant = await catalog.getModelVariant("qwen2.5-0.5b-instruct-generic-cpu:1");
    expect(variant).toBeInstanceOf(Model);
    const info = variant.info;
    expect(info.id).toBe("qwen2.5-0.5b-instruct-generic-cpu:1");
    expect(info.alias).toBe("qwen2.5-0.5b-instruct");
  });

  it("getCachedModels returns Model instances", async () => {
    const cached = await catalog.getCachedModels();
    expect(cached.length).toBeGreaterThanOrEqual(0);
    for (const m of cached) {
      expect(m).toBeInstanceOf(Model);
    }
  });

  it("getLoadedModels returns an empty array (nothing loaded)", async () => {
    const loaded = await catalog.getLoadedModels();
    expect(loaded).toEqual([]);
  });
});
