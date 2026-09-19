import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";

import { afterAll, beforeAll, describe, expect, it } from "vitest";

import { type Catalog, unwrapNativeCatalog } from "../src/catalog.js";
import {
  CatalogType,
  FoundryLocalManager,
  ModelInfoIntProperty,
  ModelInfoStringProperty,
  MutableModelInfo,
} from "../src/index.js";
import { Model } from "../src/model.js";
import { unwrapMutableModelInfo } from "../src/modelInfo.js";

import {
  type CacheOnlyManagerFixture,
  haveNativePrereqs,
  nativePrereqsDiagnostic,
  setupCacheOnlyManager,
  teardownCacheOnlyManager,
} from "./_fixtures/cacheOnlyManager.js";

const describeIfBuilt = haveNativePrereqs ? describe : describe.skip;

if (!haveNativePrereqs) {
  console.warn(`[BYOM tests] SKIPPED — ${nativePrereqsDiagnostic}`);
}

describeIfBuilt("BYOM local catalog", () => {
  let fixture: CacheOnlyManagerFixture;
  let publicCatalog: Catalog;
  let localCatalog: Catalog;
  let modelPath: string;
  const registeredIds = new Set<string>();

  beforeAll(() => {
    fixture = setupCacheOnlyManager({ appName: "foundry-local-js-sdk-v2-byom-tests" });
    publicCatalog = fixture.manager.catalog;
    localCatalog = fixture.manager.getCatalog(CatalogType.Local);
    modelPath = join(fixture.tmpDir, "byom-assets");
    mkdirSync(modelPath);
    writeFileSync(join(modelPath, "genai_config.json"), '{"model":{"type":"phi3","context_length":4096}}');
  });

  afterAll(async () => {
    for (const modelId of registeredIds) {
      try {
        await localCatalog.unregisterModel(modelId);
      } catch {
        // Best-effort cleanup for registrations a failed assertion may have already removed.
      }
    }
    teardownCacheOnlyManager(fixture);
  });

  it("keeps the catalog property backward-compatible with the default public catalog", () => {
    expect(publicCatalog).toBe(fixture.manager.getCatalog());
    expect(publicCatalog).toBe(fixture.manager.getCatalog(CatalogType.Public));
    expect(localCatalog).toBe(fixture.manager.getCatalog(CatalogType.Local));
    expect(localCatalog).not.toBe(publicCatalog);
    expect(localCatalog.name).toBe("local");
  });

  it("registers and unregisters a local model asynchronously with copied typed metadata", async () => {
    const modelId = "js-byom-async-generic-cpu:1";
    registeredIds.add(modelId);
    using metadata = new MutableModelInfo()
      .setStringProperty(ModelInfoStringProperty.Task, "chat-completion")
      .setStringProperty(ModelInfoStringProperty.DisplayName, "JS BYOM Async")
      .setStringProperty(ModelInfoStringProperty.InputModalities, "text")
      .setStringProperty(ModelInfoStringProperty.ToolCallStart, "<tool>")
      .setStringProperty(ModelInfoStringProperty.ToolCallEnd, "</tool>")
      .setStringProperty(ModelInfoStringProperty.ReasoningStart, "<think>")
      .setStringProperty(ModelInfoStringProperty.ReasoningEnd, "</think>")
      .setIntProperty(ModelInfoIntProperty.ContextLength, 4096)
      .setIntProperty(ModelInfoIntProperty.SupportsToolCalling, 1)
      .setIntProperty(ModelInfoIntProperty.SupportsReasoning, 1)
      .setIntProperty(ModelInfoIntProperty.SupportsHybridReasoning, 0)
      .setStringProperty("custom_label", "preserved")
      .setIntProperty("custom_count", 42);

    const model = await localCatalog.registerModel(modelPath, modelId, metadata);
    expect(model).toBeInstanceOf(Model);
    expect(model.info.id).toBe(modelId);
    expect(model.info.alias).toBe("js-byom-async");
    expect(model.info.task).toBe("chat-completion");
    expect(model.info.displayName).toBe("JS BYOM Async");
    expect(model.info.contextLength).toBe(4096);
    expect(model.info.toolCallStart).toBe("<tool>");
    expect(model.info.toolCallEnd).toBe("</tool>");
    expect(model.info.reasoningStart).toBe("<think>");
    expect(model.info.reasoningEnd).toBe("</think>");
    expect(model.info.supportsToolCalling).toBe(true);
    expect(model.info.supportsReasoning).toBe(true);
    expect(model.info.supportsHybridReasoning).toBe(false);
    expect(model.getStringProperty("custom_label")).toBe("preserved");
    expect(model.getIntProperty("custom_count")).toBe(42);
    expect(model.isCached).toBe(true);
    expect(await model.isLoaded()).toBe(false);
    expect(model.path).toBe(modelPath);

    metadata.dispose();
    expect(model.info.id).toBe(modelId);
    const roundTrip = await localCatalog.getModelVariant(modelId);
    expect(roundTrip.info.toolCallStart).toBe("<tool>");
    expect(roundTrip.info.toolCallEnd).toBe("</tool>");
    expect(roundTrip.info.reasoningStart).toBe("<think>");
    expect(roundTrip.info.reasoningEnd).toBe("</think>");
    expect(roundTrip.info.supportsToolCalling).toBe(true);
    expect(roundTrip.info.supportsReasoning).toBe(true);
    expect(roundTrip.info.supportsHybridReasoning).toBe(false);
    expect(roundTrip.getStringProperty("custom_label")).toBe("preserved");
    expect(roundTrip.getIntProperty("custom_count")).toBe(42);
    await localCatalog.unregisterModel(modelId);
    registeredIds.delete(modelId);
    await expect(localCatalog.getModelVariant(modelId)).rejects.toThrow(modelId);
    expect(model.info.id).toBe(modelId);
  });

  it("provides explicit event-loop-blocking sync registration APIs", async () => {
    const modelId = "js-byom-sync:2";
    registeredIds.add(modelId);
    using metadata = new MutableModelInfo().setStringProperty(ModelInfoStringProperty.Task, "chat-completion");

    const model = localCatalog.registerModelSync(modelPath, modelId, metadata);
    expect(model.info.id).toBe(modelId);
    localCatalog.unregisterModelSync(modelId);
    registeredIds.delete(modelId);
    await expect(localCatalog.getModelVariant(modelId)).rejects.toThrow(modelId);
    expect(model.info.id).toBe(modelId);
  });

  it("rejects mutation through the public catalog", async () => {
    using metadata = new MutableModelInfo().setStringProperty(ModelInfoStringProperty.Task, "chat-completion");
    await expect(publicCatalog.registerModel(modelPath, "public-rejected:1", metadata)).rejects.toMatchObject({
      name: "FoundryLocalError",
    });
    await expect(publicCatalog.unregisterModel("public-rejected:1")).rejects.toMatchObject({
      name: "FoundryLocalError",
    });
  });

  it("makes mutable metadata disposal idempotent and rejects post-dispose use", async () => {
    const metadata = new MutableModelInfo();
    metadata.dispose();
    metadata.dispose();
    expect(metadata.disposed).toBe(true);
    expect(() => metadata.setStringProperty(ModelInfoStringProperty.Task, "chat-completion")).toThrow(/disposed/);
    expect(() => localCatalog.registerModelSync(modelPath, "disposed:1", metadata)).toThrow(/non-disposed/);
  });

  it("converts snapshot exceptions before sync or async registration starts", async () => {
    using metadata = new MutableModelInfo().setStringProperty(ModelInfoStringProperty.Task, "chat-completion");
    const nativeMetadata = unwrapMutableModelInfo(metadata);

    let workerStarted = false;
    nativeMetadata.failNextSnapshotForTest();
    let asyncError: unknown;
    try {
      unwrapNativeCatalog(localCatalog).registerModel(modelPath, "snapshot-failure-async:1", nativeMetadata, () => {
        workerStarted = true;
      });
    } catch (error) {
      asyncError = error;
    }
    expect(asyncError).toMatchObject({
      name: "FoundryLocalError",
      code: 2,
      message: expect.stringMatching(/Injected ModelInfo snapshot failure/),
    });
    await new Promise((resolve) => setTimeout(resolve, 50));
    expect(workerStarted).toBe(false);

    nativeMetadata.failNextSnapshotForTest();
    let syncError: unknown;
    try {
      localCatalog.registerModelSync(modelPath, "snapshot-failure-sync:1", metadata);
    } catch (error) {
      syncError = error;
    }
    expect(syncError).toMatchObject({
      name: "FoundryLocalError",
      code: 2,
      message: expect.stringMatching(/Injected ModelInfo snapshot failure/),
    });
  });

  it("rejects int64 metadata that cannot be represented as a safe JavaScript number", async () => {
    const propertyModelId = "js-byom-unsafe-int64-property:1";
    registeredIds.add(propertyModelId);
    using metadata = new MutableModelInfo()
      .setStringProperty(ModelInfoStringProperty.Task, "chat-completion")
      .setIntProperty("custom_safe", Number.MAX_SAFE_INTEGER);
    const nativeMetadata = unwrapMutableModelInfo(metadata);
    nativeMetadata.setIntPropertyForTest("custom_unsafe", 9_007_199_254_740_992n);

    const propertyModel = await localCatalog.registerModel(modelPath, propertyModelId, metadata);
    expect(propertyModel.getIntProperty("custom_safe")).toBe(Number.MAX_SAFE_INTEGER);
    expect(() => propertyModel.getIntProperty("custom_unsafe")).toThrow(RangeError);
    expect(() => propertyModel.getIntProperty("missing", Number.MAX_SAFE_INTEGER + 1)).toThrow(RangeError);
    expect(() => propertyModel.getIntProperty("missing", Number.NaN)).toThrow(RangeError);
    await localCatalog.unregisterModel(propertyModelId);
    registeredIds.delete(propertyModelId);

    const snapshotModelId = "js-byom-unsafe-int64-snapshot:1";
    registeredIds.add(snapshotModelId);
    using snapshotMetadata = new MutableModelInfo().setStringProperty(ModelInfoStringProperty.Task, "chat-completion");
    unwrapMutableModelInfo(snapshotMetadata).setIntPropertyForTest(
      ModelInfoIntProperty.ContextLength,
      9_007_199_254_740_992n,
    );
    const snapshotModel = await localCatalog.registerModel(modelPath, snapshotModelId, snapshotMetadata);
    expect(() => snapshotModel.info).toThrow(RangeError);
    await localCatalog.unregisterModel(snapshotModelId);
    registeredIds.delete(snapshotModelId);
  });

  it("rejects embedded NULs before native catalog or metadata dispatch", async () => {
    using metadata = new MutableModelInfo();
    expect(() => metadata.setStringProperty("bad\0key", "value")).toThrow(/embedded NUL/);
    expect(() => metadata.setStringProperty("key", "bad\0value")).toThrow(/embedded NUL/);
    expect(() => metadata.setIntProperty("bad\0key", 1)).toThrow(/embedded NUL/);
    expect(() => localCatalog.registerModelSync(`${modelPath}\0suffix`, "nul-path:1", metadata)).toThrow(
      /embedded NUL/,
    );
    expect(() => localCatalog.registerModelSync(modelPath, "nul-id:1\0suffix", metadata)).toThrow(/embedded NUL/);
    await expect(localCatalog.getModel("alias\0suffix")).rejects.toThrow(/embedded NUL/);
    await expect(localCatalog.getModelVariant("model:1\0suffix")).rejects.toThrow(/embedded NUL/);
    await expect(localCatalog.getModelVersions("alias", "name\0suffix")).rejects.toThrow(/embedded NUL/);
    await expect(localCatalog.unregisterModel("model:1\0suffix")).rejects.toThrow(/embedded NUL/);
  });
});

describe("BYOM TypeScript validation", () => {
  it("rejects invalid catalog selectors before native dispatch", () => {
    expect(() => Reflect.apply(FoundryLocalManager.prototype.getCatalog, {}, [999])).toThrow(/Catalog type must be/);
  });

  it("exports every property key with its canonical native spelling", () => {
    expect(ModelInfoStringProperty.Task).toBe("task");
    expect(ModelInfoStringProperty.ExecutionProvider).toBe("execution_provider");
    expect(ModelInfoIntProperty.FileSizeMb).toBe("filesize_mb");
    expect(ModelInfoIntProperty.SupportsToolCalling).toBe("supports_tool_calling");
  });
});
