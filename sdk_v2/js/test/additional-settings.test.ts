// Verifies `FoundryLocalConfig.additionalSettings` plumbing through the
// native addon (manager.cc -> Configuration::SetAdditionalOptions). Each
// vitest file runs in its own worker process, so the Manager singleton
// invariant is per-file.
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { afterAll, describe, expect, it } from "vitest";

import { FoundryLocalManager } from "../src/foundryLocalManager.js";

import { haveNativePrereqs, nativePrereqsDiagnostic } from "./_fixtures/cacheOnlyManager.js";

const describeIfBuilt = haveNativePrereqs ? describe : describe.skip;

if (!haveNativePrereqs) {
  // eslint-disable-next-line no-console
  console.warn(`[additionalSettings tests] SKIPPED — ${nativePrereqsDiagnostic}`);
}

const FIXTURE_CATALOG = {
  version: 1,
  savedAtUnix: 1713800000,
  models: [],
};

describeIfBuilt("FoundryLocalConfig.additionalSettings", () => {
  let tmpDir: string | undefined;
  let manager: FoundryLocalManager | undefined;

  afterAll(() => {
    if (manager && !manager.disposed) manager.dispose();
    if (tmpDir) rmSync(tmpDir, { recursive: true, force: true });
  });

  it("rejects non-string property values with TypeError", () => {
    expect(() =>
      FoundryLocalManager.create({
        appName: "foundry-local-js-sdk-v2-additional-settings-bad",
        // @ts-expect-error — exercising runtime guard
        additionalSettings: { foo: 42 },
      }),
    ).toThrow(TypeError);
  });

  it("rejects a non-object additionalSettings value with TypeError", () => {
    expect(() =>
      FoundryLocalManager.create({
        appName: "foundry-local-js-sdk-v2-additional-settings-bad2",
        // @ts-expect-error — exercising runtime guard
        additionalSettings: "not an object",
      }),
    ).toThrow(TypeError);
  });

  it("accepts a populated key/value string object", () => {
    tmpDir = mkdtempSync(join(tmpdir(), "fl-js-v2-addsettings-"));
    writeFileSync(join(tmpDir, "foundry.modelinfo.json"), JSON.stringify(FIXTURE_CATALOG));
    manager = FoundryLocalManager.create({
      appName: "foundry-local-js-sdk-v2-additional-settings-ok",
      modelCacheDir: tmpDir,
      serviceEndpoint: "http://127.0.0.1:12345",
      additionalSettings: { someOption: "someValue", otherOption: "42" },
    });
    expect(manager).toBeInstanceOf(FoundryLocalManager);
  });
});
