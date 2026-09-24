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
import { type NativeSession, getAddon } from "../src/detail/native.js";
import { FoundryLocalManager } from "../src/foundryLocalManager.js";
import { Item } from "../src/items.js";
import { Model, unwrapNativeModel } from "../src/model.js";
import { MutableModelInfo, unwrapMutableModelInfo } from "../src/modelInfo.js";
import { Request, unwrapNativeRequest } from "../src/request.js";
import { ChatSession } from "../src/session.js";
import { CatalogType } from "../src/types.js";

import { haveNativePrereqs, nativePrereqsDiagnostic } from "./_fixtures/cacheOnlyManager.js";
import { haveTestModelCache, setupRealModelManager, teardownRealModelManager } from "./_fixtures/realModelManager.js";

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

  it("retained model calls fail safely after manager disposal", async () => {
    await withByomAssets(async (modelCacheDir, modelPath) => {
      const first = FoundryLocalManager.create({ appName: "retained-model", modelCacheDir });
      using metadata = new MutableModelInfo().setStringProperty("task", "chat-completion");
      const retainedModel = first
        .getCatalog(CatalogType.Local)
        .registerModelSync(modelPath, "retained-model:1", metadata);
      first.dispose();

      expect(() => retainedModel.info).toThrowError(
        expect.objectContaining({ name: "FoundryLocalError", code: 4, message: expect.stringMatching(/disposed/i) }),
      );
      expect(() => retainedModel.isCached).toThrowError(
        expect.objectContaining({ name: "FoundryLocalError", code: 4, message: expect.stringMatching(/disposed/i) }),
      );

      using second = FoundryLocalManager.create({ appName: "after-retained-model", modelCacheDir });
      await second.getCatalog(CatalogType.Local).unregisterModel("retained-model:1");
    });
  });

  it("keeps native model operations alive when the manager is disposed after the worker starts", async () => {
    await withByomAssets(async (modelCacheDir, modelPath) => {
      const first = FoundryLocalManager.create({ appName: "dispose-during-model-operation", modelCacheDir });
      using metadata = new MutableModelInfo()
        .setStringProperty("task", "chat-completion")
        .setStringProperty("custom_label", "recreated")
        .setIntProperty("custom_count", 73);
      const model = first
        .getCatalog(CatalogType.Local)
        .registerModelSync(modelPath, "dispose-during-model-operation:1", metadata);
      if (!(model instanceof Model)) throw new Error("Expected registration to return a Model");

      await expect(unwrapNativeModel(model).unload(() => first.dispose())).resolves.toBeUndefined();

      using second = FoundryLocalManager.create({ appName: "after-model-operation", modelCacheDir });
      const secondCatalog = second.getCatalog(CatalogType.Local);
      const recreated = await secondCatalog.getModelVariant("dispose-during-model-operation:1");
      expect(recreated.info.id).toBe("dispose-during-model-operation:1");
      expect(recreated.getStringProperty("custom_label")).toBe("recreated");
      expect(recreated.getIntProperty("custom_count")).toBe(73);
      await secondCatalog.unregisterModel("dispose-during-model-operation:1");
    });
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

  it("keeps EP registration alive when a progress callback disposes the manager", async () => {
    const manager = new (getAddon().Manager)({ appName: "dispose-during-ep-download" });
    let progressCalled = false;
    await expect(
      manager.downloadAndRegisterEps(
        ["FoundryLocalTestExecutionProvider"],
        () => {
          progressCalled = true;
          manager.dispose();
        },
        true,
      ),
    ).resolves.toBeUndefined();
    expect(progressCalled).toBe(true);
    expect(manager.isDisposed()).toBe(true);

    const next = new (getAddon().Manager)({ appName: "after-dispose-during-ep-download" });
    next.dispose();
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

describe.skipIf(!haveTestModelCache)("FoundryLocalManager.dispose with active sessions", () => {
  it(
    "retained session calls fail safely after manager disposal",
    async () => {
      const fixture = await setupRealModelManager({ appName: "dispose-retained-session" });
      const session = new ChatSession(fixture.model);
      try {
        fixture.manager.dispose();
        expect(() => session.turnCount).toThrowError(
          expect.objectContaining({ name: "FoundryLocalError", code: 4, message: expect.stringMatching(/disposed/i) }),
        );
      } finally {
        session.dispose();
        teardownRealModelManager(fixture);
      }
    },
    5 * 60_000,
  );

  it(
    "keeps an admitted request alive when the manager is disposed",
    async () => {
      const fixture = await setupRealModelManager({ appName: "dispose-during-session-request" });
      const session = new ChatSession(fixture.model);
      try {
        const request = new Request()
          .addItem(Item.userMessage("Reply with ok."))
          .setOptions({ search: { maxOutputTokens: 16, temperature: 0 } });
        const response = session.processRequest(request);
        fixture.manager.dispose();

        await expect(response).resolves.toMatchObject({ output: expect.any(Array) });
      } finally {
        session.dispose();
        teardownRealModelManager(fixture);
      }
    },
    5 * 60_000,
  );

  it(
    "keeps an admitted streaming request alive when the manager is disposed",
    async () => {
      const fixture = await setupRealModelManager({ appName: "dispose-during-manager-stream" });
      const session = new ChatSession(fixture.model);
      try {
        const request = new Request()
          .addItem(Item.userMessage("Reply with ok."))
          .setOptions({ search: { maxOutputTokens: 16, temperature: 0 } });
        const stream = session.processStreamingRequest(request);
        fixture.manager.dispose();

        await expect(stream.response).resolves.toMatchObject({ output: expect.any(Array) });
      } finally {
        session.dispose();
        teardownRealModelManager(fixture);
      }
    },
    5 * 60_000,
  );

  it(
    "keeps an admitted request alive when the session is disposed",
    async () => {
      const fixture = await setupRealModelManager({ appName: "dispose-during-session-request" });
      const session = new ChatSession(fixture.model);
      try {
        const request = new Request()
          .addItem(Item.userMessage("Reply with ok."))
          .setOptions({ search: { maxOutputTokens: 16, temperature: 0 } });
        const response = session.processRequest(request);
        session.dispose();

        await expect(response).resolves.toMatchObject({ output: expect.any(Array) });
      } finally {
        session.dispose();
        teardownRealModelManager(fixture);
      }
    },
    5 * 60_000,
  );

  it(
    "keeps an admitted streaming request alive when the session is disposed",
    async () => {
      const fixture = await setupRealModelManager({ appName: "dispose-during-session-stream" });
      const session = new ChatSession(fixture.model);
      try {
        const request = new Request()
          .addItem(Item.userMessage("Reply with ok."))
          .setOptions({ search: { maxOutputTokens: 16, temperature: 0 } });
        const stream = session.processStreamingRequest(request);
        session.dispose();

        await expect(stream.response).resolves.toMatchObject({ output: expect.any(Array) });
      } finally {
        session.dispose();
        teardownRealModelManager(fixture);
      }
    },
    5 * 60_000,
  );

  it.skipIf((globalThis as { gc?: () => void }).gc === undefined)(
    "keeps an admitted request alive when the session is garbage collected",
    async () => {
      const fixture = await setupRealModelManager({ appName: "gc-during-session-request" });
      try {
        expect(globalThis.gc).toBeTypeOf("function");
        const gc = (globalThis as { gc: () => void }).gc;
        const collected = new Set<string>();
        const registry = new FinalizationRegistry<string>((label) => collected.add(label));
        let session: ChatSession | undefined = new ChatSession(fixture.model);
        let nativeSession: NativeSession | undefined = (session as unknown as { native: NativeSession }).native;
        const weakSession = new WeakRef(session);
        const weakNativeSession = new WeakRef(nativeSession);
        registry.register(session, "session");
        registry.register(nativeSession, "nativeSession");
        const request = new Request()
          .addItem(Item.userMessage("Reply with ok."))
          .setOptions({ search: { maxOutputTokens: 16, temperature: 0 } });
        let resolveCollectionCheck!: () => void;
        let rejectCollectionCheck!: (error: unknown) => void;
        const collectionCheck = new Promise<void>((resolve, reject) => {
          resolveCollectionCheck = resolve;
          rejectCollectionCheck = reject;
        });
        let responseSettled = false;
        const response = nativeSession.processRequest(unwrapNativeRequest(request), (release) => {
          void (async () => {
            try {
              for (let attempt = 0; attempt < 100 && collected.size < 2; attempt++) {
                gc();
                await new Promise<void>((resolve) => setImmediate(resolve));
              }
              expect(responseSettled).toBe(false);
              expect(collected).toEqual(new Set(["session", "nativeSession"]));
              expect(weakSession.deref()).toBeUndefined();
              expect(weakNativeSession.deref()).toBeUndefined();
              resolveCollectionCheck();
            } catch (error) {
              rejectCollectionCheck(error);
            } finally {
              release();
            }
          })();
        });
        void response.then(
          () => {
            responseSettled = true;
          },
          () => {
            responseSettled = true;
          },
        );
        session = undefined;
        nativeSession = undefined;

        await collectionCheck;
        await expect(response).resolves.toMatchObject({ output: expect.any(Array) });
      } finally {
        teardownRealModelManager(fixture);
      }
    },
    5 * 60_000,
  );
});
