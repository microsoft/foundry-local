// Manager-only tests for the v2 JS SDK. Uses the shared cache-only fixture
// helper. One Manager per file: created in `beforeAll`, disposed in
// `afterAll`. See `_fixtures/cacheOnlyManager.ts` for the policy.
//
// Dispose lifecycle tests live in `manager-dispose.test.ts` so this file
// can keep its Manager alive across every test.
import { afterAll, beforeAll, describe, expect, it } from "vitest";

import { FoundryLocalManager } from "../src/foundryLocalManager.js";

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
  console.warn(`[FoundryLocalManager tests] SKIPPED — ${nativePrereqsDiagnostic}`);
}

describeIfBuilt("FoundryLocalManager", () => {
  let fixture: CacheOnlyManagerFixture;

  beforeAll(() => {
    fixture = setupCacheOnlyManager({ appName: "foundry-local-js-sdk-v2-manager-tests" });
  });
  afterAll(() => {
    teardownCacheOnlyManager(fixture);
  });

  it("is an instance of FoundryLocalManager", () => {
    expect(fixture.manager).toBeInstanceOf(FoundryLocalManager);
  });

  it("rejects missing or empty appName before native construction", () => {
    expect(() => FoundryLocalManager.create({ appName: "" })).toThrow(TypeError);
    // @ts-expect-error — exercising runtime guard
    expect(() => FoundryLocalManager.create({})).toThrow(TypeError);
    // @ts-expect-error — exercising runtime guard
    expect(() => FoundryLocalManager.create(undefined)).toThrow(TypeError);
  });

  it("rejects unknown config properties (typo guard)", () => {
    // Common typo: `cachePath` instead of `modelCacheDir`. Must throw before native construction so the
    // user sees the real problem instead of a silently-ignored option and a confusing downstream failure.
    expect(() =>
      FoundryLocalManager.create({
        appName: "foundry-local-js-sdk-v2-manager-unknown-key",
        // @ts-expect-error — exercising runtime guard for typo'd option
        cachePath: "D:/does/not/matter",
      }),
    ).toThrow(/cachePath/);
  });

  it("urls returns an empty array when no service is running", () => {
    expect(fixture.manager.urls).toEqual([]);
    expect(fixture.manager.isWebServiceRunning).toBe(false);
  });

  it("catalog returns a cached Catalog reference across calls", () => {
    const a = fixture.manager.catalog;
    const b = fixture.manager.catalog;
    expect(a).toBe(b);
    expect(typeof a.name).toBe("string");
    expect(a.name.length).toBeGreaterThan(0);
  });

  it("disposed is false while the manager is alive", () => {
    expect(fixture.manager.disposed).toBe(false);
  });

  it("isShutdownRequested is false on a fresh manager", () => {
    expect(fixture.manager.isShutdownRequested).toBe(false);
  });

  it("isEpDownloadInProgress returns a boolean", () => {
    expect(typeof fixture.manager.isEpDownloadInProgress()).toBe("boolean");
  });

  it("discoverEps returns an array of EpInfo records", () => {
    const eps = fixture.manager.discoverEps();
    expect(Array.isArray(eps)).toBe(true);
    for (const ep of eps) {
      expect(typeof ep.name).toBe("string");
      expect(typeof ep.isRegistered).toBe("boolean");
    }
  });
});
