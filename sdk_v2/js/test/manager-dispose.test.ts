// Dispose-lifecycle tests for the v2 SDK Manager. Isolated from the rest of
// the test suite so this file owns the only test fixtures that construct
// multiple Managers sequentially.
//
// Each test gets a fresh Manager (constructed inline, no shared fixture) so
// the dispose-then-call assertions don't poison sibling tests.
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { describe, expect, it } from "vitest";

import { unwrapNativeCatalog } from "../src/catalog.js";
import { isFoundryLocalError } from "../src/detail/errors.js";
import { FoundryLocalManager } from "../src/foundryLocalManager.js";
import { MutableModelInfo, unwrapMutableModelInfo } from "../src/modelInfo.js";
import { CatalogType } from "../src/types.js";

import { haveNativePrereqs, nativePrereqsDiagnostic } from "./_fixtures/cacheOnlyManager.js";

const describeIfBuilt = haveNativePrereqs ? describe : describe.skip;

if (!haveNativePrereqs) {
  // eslint-disable-next-line no-console
  console.warn(`[FoundryLocalManager.dispose tests] SKIPPED — ${nativePrereqsDiagnostic}`);
}

function freshManager(appNameSuffix: string): FoundryLocalManager {
  return FoundryLocalManager.create({ appName: `foundry-local-js-sdk-v2-dispose-${appNameSuffix}` });
}

function withByomAssets<T>(run: (modelCacheDir: string, modelPath: string) => Promise<T>): Promise<T> {
  const modelCacheDir = mkdtempSync(join(tmpdir(), "foundry-local-js-sdk-v2-dispose-"));
  const modelPath = join(modelCacheDir, "assets");
  mkdirSync(modelPath);
  writeFileSync(join(modelPath, "genai_config.json"), '{"model":{"type":"phi3","context_length":4096}}');
  return run(modelCacheDir, modelPath).finally(() => rmSync(modelCacheDir, { recursive: true, force: true }));
}

describeIfBuilt("FoundryLocalManager.dispose", () => {
  it("disposed is false on a fresh manager", () => {
    const mgr = freshManager("fresh");
    try {
      expect(mgr.disposed).toBe(false);
    } finally {
      mgr.dispose();
    }
  });

  it("dispose() is idempotent — calling twice does not throw", () => {
    const mgr = freshManager("idempotent");
    mgr.dispose();
    expect(mgr.disposed).toBe(true);
    expect(() => mgr.dispose()).not.toThrow();
    expect(mgr.disposed).toBe(true);
  });

  it("retained catalog calls fail safely after manager disposal", () => {
    const first = freshManager("retained-catalog");
    const retainedCatalog = first.catalog;
    first.dispose();

    expect(() => retainedCatalog.name).toThrowError(
      expect.objectContaining({
        name: "FoundryLocalError",
        code: 4,
        message: expect.stringMatching(/disposed/i),
      }),
    );
    const second = freshManager("after-retained-catalog");
    second.dispose();
  });

  it("keeps native registration alive when the manager is disposed after the worker starts", async () => {
    await withByomAssets(async (modelCacheDir, modelPath) => {
      const first = FoundryLocalManager.create({
        appName: "dispose-during-register",
        modelCacheDir,
      });
      const catalog = first.getCatalog(CatalogType.Local);
      using metadata = new MutableModelInfo().setStringProperty("task", "chat-completion");

      const registration = unwrapNativeCatalog(catalog).registerModel(
        modelPath,
        "dispose-during-register:1",
        unwrapMutableModelInfo(metadata),
        () => first.dispose(),
      );
      await expect(registration).rejects.toMatchObject({
        name: "FoundryLocalError",
        code: 4,
        message: expect.stringMatching(/disposed/i),
      });

      using second = FoundryLocalManager.create({
        appName: "after-dispose-during-register",
        modelCacheDir,
      });
      const secondCatalog = second.getCatalog(CatalogType.Local);
      const registered = await secondCatalog.getModelVariant("dispose-during-register:1");
      expect(registered.info.id).toBe("dispose-during-register:1");
      await secondCatalog.unregisterModel("dispose-during-register:1");
    });
  });

  it("keeps native unregistration alive when the manager is disposed after the worker starts", async () => {
    await withByomAssets(async (modelCacheDir, modelPath) => {
      const first = FoundryLocalManager.create({
        appName: "dispose-during-unregister",
        modelCacheDir,
      });
      const catalog = first.getCatalog(CatalogType.Local);
      using metadata = new MutableModelInfo().setStringProperty("task", "chat-completion");
      catalog.registerModelSync(modelPath, "dispose-during-unregister:1", metadata);

      const unregistration = unwrapNativeCatalog(catalog).unregisterModel("dispose-during-unregister:1", () =>
        first.dispose(),
      );
      await expect(unregistration).resolves.toBeUndefined();

      using second = FoundryLocalManager.create({
        appName: "after-dispose-during-unregister",
        modelCacheDir,
      });
      await expect(second.getCatalog(CatalogType.Local).getModelVariant("dispose-during-unregister:1")).rejects.toThrow(
        /dispose-during-unregister:1/,
      );
    });
  });

  it("reading urls after dispose() returns the cleared cache (no native call)", () => {
    const mgr = freshManager("post-dispose-urls");
    mgr.dispose();
    expect(mgr.urls).toEqual([]);
    expect(mgr.isWebServiceRunning).toBe(false);
  });

  it("accessing catalog after dispose() throws a tagged FoundryLocalError", () => {
    const mgr = freshManager("post-dispose-catalog");
    mgr.dispose();
    try {
      // Property access triggers the native getCatalog call.
      void mgr.catalog;
      throw new Error("expected catalog accessor to throw");
    } catch (err) {
      expect(isFoundryLocalError(err)).toBe(true);
      const fle = err as Error & { code: number };
      expect(fle.code).toBe(4); // FOUNDRY_LOCAL_ERROR_INVALID_USAGE
      expect(fle.message).toMatch(/disposed/i);
    }
  });

  it("Symbol.dispose is wired and idempotent", () => {
    const mgr = freshManager("symbol-dispose");
    mgr[Symbol.dispose]();
    expect(mgr.disposed).toBe(true);
    expect(() => mgr[Symbol.dispose]()).not.toThrow();
    expect(mgr.disposed).toBe(true);
  });

  it("`using` declaration disposes at scope exit", () => {
    let captured: FoundryLocalManager | undefined;
    {
      using mgr = freshManager("using");
      captured = mgr;
      expect(mgr.disposed).toBe(false);
    }
    expect(captured?.disposed).toBe(true);
  });
});
