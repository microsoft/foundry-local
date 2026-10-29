import { expect, it, vi } from "vitest";

const native = vi.hoisted(() => ({
  getWebServiceEndpoints: vi.fn<() => string[]>(() => []),
  startWebService: vi.fn(),
  isDisposed: vi.fn(() => false),
  dispose: vi.fn(),
}));

vi.mock("../src/detail/native.js", () => ({
  getAddon: () => ({
    Manager: class {
      getWebServiceEndpoints = native.getWebServiceEndpoints;
      startWebService = native.startWebService;
      isDisposed = native.isDisposed;
      dispose = native.dispose;
    },
  }),
  configureNativeLoader: vi.fn(),
  getPreloadedLibraryPath: () => undefined,
}));

import { FoundryLocalManager } from "../src/foundryLocalManager.js";

it("observes listener changes from another client without starting the service", () => {
  using manager = FoundryLocalManager.create({ appName: "listener-observer" });
  expect(manager.urls).toEqual([]);
  native.getWebServiceEndpoints.mockReturnValue(["http://127.0.0.1:12345"]);
  expect(manager.urls).toEqual(["http://127.0.0.1:12345"]);
  expect(manager.isWebServiceRunning).toBe(true);
  native.getWebServiceEndpoints.mockReturnValue([]);
  expect(manager.urls).toEqual([]);
  expect(manager.isWebServiceRunning).toBe(false);
  expect(native.startWebService).not.toHaveBeenCalled();
});
