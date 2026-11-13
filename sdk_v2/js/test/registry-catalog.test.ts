import { beforeEach, describe, expect, it, vi } from "vitest";

import type { NativeCatalog } from "../src/detail/native.js";

const native = vi.hoisted(() => ({
  getCatalog: vi.fn<(type?: number, options?: Readonly<Record<string, string>>) => NativeCatalog>(),
  isDisposed: vi.fn(() => false),
  dispose: vi.fn(),
}));

vi.mock("../src/detail/native.js", () => ({
  getAddon: () => ({
    Manager: class {
      getCatalog = native.getCatalog;
      isDisposed = native.isDisposed;
      dispose = native.dispose;
    },
  }),
  configureNativeLoader: vi.fn(),
  getPreloadedLibraryPath: () => undefined,
}));

import { FoundryLocalManager } from "../src/foundryLocalManager.js";
import { CatalogType } from "../src/types.js";

function fakeCatalog(name: string): NativeCatalog {
  return {
    getName: () => name,
    getModels: () => [],
    getCachedModels: () => [],
    getLoadedModels: () => [],
    getModel: () => undefined,
    getModelVariant: () => undefined,
    getLatestVersion: () => undefined,
    getModelVersions: async () => [],
    registerModel: vi.fn(),
    registerModelSync: vi.fn(),
    unregisterModel: vi.fn(),
    unregisterModelSync: vi.fn(),
  };
}

beforeEach(() => {
  vi.clearAllMocks();
  native.isDisposed.mockReturnValue(false);
  native.getCatalog.mockImplementation((_type, options) => fakeCatalog(options?.name ?? "public"));
});

describe("registry catalog binding", () => {
  it("copies options, caches reordered options, and lists through the selected catalog", async () => {
    using manager = FoundryLocalManager.create({ appName: "registry-test" });
    const options = { name: "registry-a", credential: "azure_cli" };
    const catalog = manager.getCatalog(CatalogType.AzureRegistry, options);
    options.name = "registry-b";
    expect(native.getCatalog).toHaveBeenCalledWith(CatalogType.AzureRegistry, {
      name: "registry-a",
      credential: "azure_cli",
    });
    expect(catalog.name).toBe("registry-a");
    expect(await catalog.getModels()).toEqual([]);
    expect(manager.getCatalog(CatalogType.AzureRegistry, { credential: "azure_cli", name: "registry-a" })).toBe(
      catalog,
    );
    expect(native.getCatalog).toHaveBeenCalledTimes(1);
    expect(manager.getCatalog(CatalogType.AzureRegistry, options)).not.toBe(catalog);
    expect(
      manager.getCatalog(CatalogType.AzureRegistry, { name: "registry-a", credential: "workload_identity" }),
    ).not.toBe(catalog);
  });

  it("preserves the existing public and local native routes", () => {
    using manager = FoundryLocalManager.create({ appName: "registry-test" });
    expect(manager.catalog).toBe(manager.getCatalog(CatalogType.Public, {}));
    manager.getCatalog(CatalogType.Local);
    expect(native.getCatalog.mock.calls).toEqual([[CatalogType.Public], [CatalogType.Local]]);
  });

  it.each([{}, { name: "bad\0name" }, { "": "value" }, { "n\0ame": "value" }])(
    "rejects invalid options or forwards missing options for native validation: %j",
    (options) => {
      using manager = FoundryLocalManager.create({ appName: "registry-test" });
      if (Object.keys(options).length === 0) {
        manager.getCatalog(CatalogType.AzureRegistry, options);
        expect(native.getCatalog).toHaveBeenCalledWith(CatalogType.AzureRegistry, {});
      } else {
        expect(() => manager.getCatalog(CatalogType.AzureRegistry, options)).toThrow(TypeError);
        expect(native.getCatalog).not.toHaveBeenCalled();
      }
    },
  );

  it("propagates native errors without caching a failed acquisition", () => {
    using manager = FoundryLocalManager.create({ appName: "registry-test" });
    native.getCatalog.mockImplementationOnce(() => {
      throw new Error("catalog options require native API version 4 or newer");
    });
    expect(() => manager.getCatalog(CatalogType.AzureRegistry, { name: "registry-a" })).toThrow(/version 4/);
    expect(manager.getCatalog(CatalogType.AzureRegistry, { name: "registry-a" }).name).toBe("registry-a");
    expect(native.getCatalog).toHaveBeenCalledTimes(2);
  });

  it("does not return a cached catalog after disposal", () => {
    using manager = FoundryLocalManager.create({ appName: "registry-test" });
    manager.getCatalog(CatalogType.AzureRegistry, { name: "registry-a" });
    manager.dispose();
    native.getCatalog.mockImplementationOnce(() => {
      throw new Error("Manager has been disposed");
    });
    expect(() => manager.getCatalog(CatalogType.AzureRegistry, { name: "registry-a" })).toThrow(/disposed/);
  });
});
