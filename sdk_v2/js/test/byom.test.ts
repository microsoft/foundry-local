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
