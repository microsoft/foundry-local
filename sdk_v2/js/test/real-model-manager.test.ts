import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

const mocks = vi.hoisted(() => ({
  existsSync: vi.fn(),
  statSync: vi.fn(),
  createManager: vi.fn(),
  metadata: { setStringProperty: vi.fn(), dispose: vi.fn() },
}));

vi.mock("node:fs", () => ({ existsSync: mocks.existsSync, statSync: mocks.statSync }));
vi.mock("../src/foundryLocalManager.js", () => ({
  FoundryLocalManager: { create: mocks.createManager },
}));
vi.mock("../src/modelInfo.js", () => ({
  ModelInfoStringProperty: { Task: "task" },
  MutableModelInfo: class {
    setStringProperty = mocks.metadata.setStringProperty;
    dispose = mocks.metadata.dispose;
  },
}));

describe("real-model manager fixture", () => {
  const publicCatalog = { getModel: vi.fn(), getModels: vi.fn() };
  const localCatalog = { getModelVariant: vi.fn(), registerModel: vi.fn() };
  const manager = { catalog: publicCatalog, getCatalog: vi.fn(), disposed: false, dispose: vi.fn() };
  const stagedModel = { isCached: true, load: vi.fn() };
  const publicModel = {
    get info() {
      if (manager.disposed) throw new Error("Manager has been disposed");
      return { name: "public-model", deviceType: "CPU", task: "chat-completion" };
    },
    isCached: false,
    load: vi.fn(),
  };

  beforeEach(() => {
    vi.resetModules();
    vi.resetAllMocks();
    vi.stubEnv("FOUNDRY_TEST_DATA_DIR", "/model-cache");
    vi.stubEnv("CI", "true");
    vi.stubEnv("TF_BUILD", undefined);
    mocks.existsSync.mockReturnValue(true);
    mocks.statSync.mockReturnValue({ isDirectory: () => true });
    mocks.createManager.mockReturnValue(manager);
    mocks.metadata.setStringProperty.mockReturnValue(mocks.metadata);
    manager.disposed = false;
    manager.dispose.mockImplementation(() => {
      manager.disposed = true;
    });
    manager.getCatalog.mockReturnValue(localCatalog);
    publicCatalog.getModel.mockResolvedValue(publicModel);
    publicCatalog.getModels.mockResolvedValue([publicModel]);
    publicModel.isCached = false;
    localCatalog.getModelVariant.mockRejectedValue(new Error("Not registered"));
    localCatalog.registerModel.mockResolvedValue(stagedModel);
  });

  afterEach(() => {
    vi.unstubAllEnvs();
  });

  it.each([
    ["ranking", "clm", "clm-v0.1-8b-mixed-generic-cpu:1"],
    ["decision", "kev", "kev-0.8b-mixed-generic-cpu:1"],
  ])("uses the staged %s package instead of an uncached public model", async (task, namePreference, modelId) => {
    const { setupRealModelManager } = await import("./_fixtures/realModelManager.js");
    const fixture = await setupRealModelManager({ task, namePreference, loadModel: false });

    expect(fixture.model).toBe(stagedModel);
    expect(fixture.catalog).toBe(localCatalog);
    expect(localCatalog.registerModel).toHaveBeenCalledWith(
      expect.stringContaining(modelId.replace(":", "-")),
      modelId,
      mocks.metadata,
    );
    expect(mocks.metadata.setStringProperty).toHaveBeenCalledWith("task", task);
    expect(mocks.metadata.dispose).toHaveBeenCalledOnce();
    expect(publicCatalog.getModel).not.toHaveBeenCalled();
    expect(stagedModel.load).not.toHaveBeenCalled();
    expect(manager.dispose).not.toHaveBeenCalled();
  });

  it("reuses a registered staged package without registering it again", async () => {
    localCatalog.getModelVariant.mockResolvedValue(stagedModel);
    const { setupRealModelManager } = await import("./_fixtures/realModelManager.js");
    const fixture = await setupRealModelManager({ task: "ranking", namePreference: "clm", loadModel: false });

    expect(fixture.model).toBe(stagedModel);
    expect(localCatalog.registerModel).not.toHaveBeenCalled();
    expect(publicCatalog.getModel).not.toHaveBeenCalled();
  });

  it("keeps the public-model fallback when no staged package exists", async () => {
    mocks.existsSync.mockImplementation((path: string) => path === "/model-cache");
    publicModel.isCached = true;
    const { setupRealModelManager } = await import("./_fixtures/realModelManager.js");
    const fixture = await setupRealModelManager({ task: "ranking", namePreference: "clm", loadModel: false });

    expect(fixture.model).toBe(publicModel);
    expect(fixture.catalog).toBe(publicCatalog);
    expect(localCatalog.registerModel).not.toHaveBeenCalled();
  });

  it.each([
    { ci: "true", loadModel: true },
    { ci: undefined, loadModel: false },
  ])("skips an uncached model before disposing its diagnostic data (%j)", async ({ ci, loadModel }) => {
    vi.stubEnv("CI", ci);
    const { setupRealModelManager, SkipFixture } = await import("./_fixtures/realModelManager.js");
    const setup = setupRealModelManager({ loadModel });

    await expect(setup).rejects.toBeInstanceOf(SkipFixture);
    await expect(setup).rejects.toThrow("Selected model 'public-model' is not in the cache");
    expect(manager.dispose).toHaveBeenCalledOnce();
    expect(publicModel.load).not.toHaveBeenCalled();
  });
});
