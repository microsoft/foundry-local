// Catalog tests for the v2 JS SDK. Uses the shared cache-only fixture
// helper so this file constructs exactly one Manager + cache directory.
import { afterAll, beforeAll, describe, expect, it } from "vitest";

import type { NativeCatalog, NativeModel, NativeModelInfo } from "../src/detail/native.js";
import type { Catalog } from "../src/catalog.js";
import { wrapNativeCatalog } from "../src/catalog.js";
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

  it("getModelVersions returns versions for a known alias and respects maxVersions", async () => {
    const nativeModelInfo: NativeModelInfo = {
      id: "phi-4-mini-instruct-generic-cpu:2",
      name: "phi-4-mini-instruct-generic-cpu",
      version: 2,
      alias: "phi-4-mini-instruct",
      uri: "azureml://registries/azureml/models/phi-4-mini-instruct-generic-cpu/versions/2",
      deviceType: "CPU",
      providerType: "FoundryLocal",
      modelType: "ONNX",
      task: "chat-completion",
      publisher: "Microsoft",
      displayName: "Phi-4 Mini Instruct",
      createdAtUnix: 1713800000,
      isTestModel: false,
    };
    const fakeNativeModel: NativeModel = {
      getInfo: () => nativeModelInfo,
      isCached: () => true,
      isLoaded: () => false,
      getPath: () => "",
      getVariants: () => [],
      selectVariant: () => {},
      load: async () => {},
      unload: async () => {},
      download: async () => {},
      removeFromCache: () => {},
    };
    const fakeNativeCatalog: NativeCatalog = {
      getName: () => "TestCatalog",
      getModels: () => [],
      getCachedModels: () => [],
      getLoadedModels: () => [],
      getModel: () => undefined,
      getModelVariant: () => undefined,
      getLatestVersion: () => undefined,
      getModelVersions: (modelAlias, modelName, maxVersions) => {
        expect(modelAlias).toBe("phi-4-mini-instruct");
        expect(modelName).toBeNull();
        expect(maxVersions).toBe(1);
        // The real native path resolves a Promise; mirror that here.
        return Promise.resolve([fakeNativeModel]);
      },
    };

    const localCatalog = wrapNativeCatalog(fakeNativeCatalog);
    const versions = await localCatalog.getModelVersions("phi-4-mini-instruct", undefined, 1);
    expect(versions).toHaveLength(1);
    for (const model of versions) {
      expect(model).toBeInstanceOf(Model);
      expect(model.info.alias).toBe("phi-4-mini-instruct");
    }
    expect(versions[0]?.info.id).toBe("phi-4-mini-instruct-generic-cpu:2");
  });
});

// Pure argument-validation: runs regardless of native build state because the
// checks happen on the JS thread before the native call is ever queued.
describe("Catalog.getModelVersions maxVersions validation", () => {
  // A fake whose getModelVersions must never be reached for invalid inputs.
  function makeGuardedCatalog(): Catalog {
    const fakeNativeCatalog: NativeCatalog = {
      getName: () => "TestCatalog",
      getModels: () => [],
      getCachedModels: () => [],
      getLoadedModels: () => [],
      getModel: () => undefined,
      getModelVariant: () => undefined,
      getLatestVersion: () => undefined,
      getModelVersions: () => {
        throw new Error("native getModelVersions must not be called for invalid maxVersions");
      },
    };
    return wrapNativeCatalog(fakeNativeCatalog);
  }

  const invalidCases: ReadonlyArray<readonly [string, number]> = [
    ["a fraction", 1.5],
    ["NaN", Number.NaN],
    ["Infinity", Number.POSITIVE_INFINITY],
    ["-Infinity", Number.NEGATIVE_INFINITY],
    ["above int32 max", 2 ** 31],
    ["below int32 min", -(2 ** 31) - 1],
  ];

  for (const [label, value] of invalidCases) {
    it(`rejects ${label} (${value})`, async () => {
      const catalog = makeGuardedCatalog();
      await expect(catalog.getModelVersions("phi-4-mini-instruct", undefined, value)).rejects.toThrow(
        /maxVersions must be an integer within the 32-bit range/,
      );
    });
  }

  const validCases: ReadonlyArray<readonly [string, number]> = [
    ["int32 max", 2 ** 31 - 1],
    ["int32 min", -(2 ** 31)],
    ["zero (no cap)", 0],
    ["negative (no cap)", -5],
  ];

  for (const [label, value] of validCases) {
    it(`accepts ${label} (${value})`, async () => {
      const fakeNativeCatalog: NativeCatalog = {
        getName: () => "TestCatalog",
        getModels: () => [],
        getCachedModels: () => [],
        getLoadedModels: () => [],
        getModel: () => undefined,
        getModelVariant: () => undefined,
        getLatestVersion: () => undefined,
        getModelVersions: (_alias, _name, maxVersions) => {
          expect(maxVersions).toBe(value);
          return Promise.resolve([]);
        },
      };
      const catalog = wrapNativeCatalog(fakeNativeCatalog);
      await expect(catalog.getModelVersions("phi-4-mini-instruct", undefined, value)).resolves.toEqual([]);
    });
  }
});
