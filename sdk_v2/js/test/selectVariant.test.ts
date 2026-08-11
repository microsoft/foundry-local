// selectVariant regression tests for the v2 JS SDK.
//
// The native model is the source of truth for metadata. The JS wrapper must not
// snapshot `info` — after `selectVariant`, `model.info` / `model.id` must reflect
// the newly selected variant. Uses a dedicated two-variant fixture (generic-gpu +
// generic-cpu under one alias) so the switch is observable.
import { afterAll, beforeAll, describe, expect, it } from "vitest";

import type { Catalog } from "../src/catalog.js";
import type { IModel } from "../src/imodel.js";
import type { Model } from "../src/model.js";

import {
  type CacheOnlyManagerFixture,
  TWO_VARIANT_MODELS,
  haveNativePrereqs,
  nativePrereqsDiagnostic,
  setupCacheOnlyManager,
  teardownCacheOnlyManager,
} from "./_fixtures/cacheOnlyManager.js";

const describeIfBuilt = haveNativePrereqs ? describe : describe.skip;

if (!haveNativePrereqs) {
  // eslint-disable-next-line no-console
  console.warn(`[selectVariant tests] SKIPPED — ${nativePrereqsDiagnostic}`);
}

const GPU_ID = "multi-variant-model-generic-gpu:1";
const CPU_ID = "multi-variant-model-generic-cpu:1";

function variantById(model: Model, id: string): IModel {
  const found = model.variants.find((v) => v.info.id === id);
  if (found === undefined) {
    throw new Error(`variant ${id} not found`);
  }
  return found;
}

describeIfBuilt("Model.selectVariant (cache-only)", () => {
  let fixture: CacheOnlyManagerFixture;
  let catalog: Catalog;

  beforeAll(() => {
    fixture = setupCacheOnlyManager({
      appName: "foundry-local-js-sdk-v2-select-variant-tests",
      models: TWO_VARIANT_MODELS,
    });
    catalog = fixture.manager.catalog;
  });

  afterAll(() => {
    teardownCacheOnlyManager(fixture);
  });

  it("exposes both variants under one alias, GPU selected by default", async () => {
    const model = (await catalog.getModel("multi-variant-model")) as Model;
    expect(model.info.alias).toBe("multi-variant-model");
    expect(model.info.id).toBe(GPU_ID);

    const variantIds = model.variants.map((v) => v.info.id).sort();
    expect(variantIds).toEqual([CPU_ID, GPU_ID]);
  });

  it("metadata reflects the selected variant after selectVariant", async () => {
    const model = (await catalog.getModel("multi-variant-model")) as Model;

    // Prime a read of the default (GPU) metadata before selecting.
    const before = model.info;
    expect(before.id).toBe(GPU_ID);

    model.selectVariant(variantById(model, CPU_ID));

    // Fresh reads must reflect the CPU variant — no stale snapshot.
    expect(model.info.id).toBe(CPU_ID);
    expect(model.id).toBe(CPU_ID);
    expect(model.info.name).toBe("multi-variant-model-generic-cpu");
    expect(model.info.deviceType).toBe("CPU");

    // The earlier value object is an independent point-in-time snapshot.
    expect(before.id).toBe(GPU_ID);
  });

  it("selecting back to the original variant refreshes metadata again", async () => {
    const model = (await catalog.getModel("multi-variant-model")) as Model;

    model.selectVariant(variantById(model, CPU_ID));
    expect(model.info.id).toBe(CPU_ID);

    model.selectVariant(variantById(model, GPU_ID));
    expect(model.info.id).toBe(GPU_ID);
    expect(model.info.deviceType).toBe("GPU");
  });
});
