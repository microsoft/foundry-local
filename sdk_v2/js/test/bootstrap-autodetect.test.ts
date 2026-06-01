// Tests for `applyBootstrapAutoDetect`. Exercised directly (no addon load required) — that's the whole point
// of extracting it as a pure helper.
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { afterEach, beforeEach, describe, expect, it } from "vitest";

import { applyBootstrapAutoDetect } from "../src/foundryLocalManager.js";

// Platform-conditional registration (rather than `describe.skip`) so the inactive
// suite is never collected — keeps vitest's skipped count at 0 on both platforms,
// so a non-zero skip count is always a real signal worth investigating.
const isWindows = process.platform === "win32";

if (isWindows) describe("applyBootstrapAutoDetect (Windows)", () => {
  let tmpDir: string;

  beforeEach(() => {
    tmpDir = mkdtempSync(join(tmpdir(), "fl-js-v2-bootstrap-"));
  });

  afterEach(() => {
    rmSync(tmpDir, { recursive: true, force: true });
  });

  it("injects Bootstrap=true when the bootstrap DLL is co-located", () => {
    writeFileSync(join(tmpDir, "Microsoft.WindowsAppRuntime.Bootstrap.dll"), "dummy");
    const result = applyBootstrapAutoDetect({ appName: "test" }, tmpDir);
    expect(result.additionalSettings).toEqual({ Bootstrap: "true" });
  });

  it("preserves existing additionalSettings while injecting Bootstrap", () => {
    writeFileSync(join(tmpDir, "Microsoft.WindowsAppRuntime.Bootstrap.dll"), "dummy");
    const result = applyBootstrapAutoDetect({ appName: "test", additionalSettings: { foo: "bar" } }, tmpDir);
    expect(result.additionalSettings).toEqual({ foo: "bar", Bootstrap: "true" });
  });

  it("does not overwrite a caller-supplied Bootstrap value", () => {
    writeFileSync(join(tmpDir, "Microsoft.WindowsAppRuntime.Bootstrap.dll"), "dummy");
    const result = applyBootstrapAutoDetect({ appName: "test", additionalSettings: { Bootstrap: "false" } }, tmpDir);
    expect(result.additionalSettings).toEqual({ Bootstrap: "false" });
  });

  it("does not mutate the caller's config or additionalSettings object", () => {
    writeFileSync(join(tmpDir, "Microsoft.WindowsAppRuntime.Bootstrap.dll"), "dummy");
    const original = { appName: "test", additionalSettings: { foo: "bar" } };
    const originalSettings = original.additionalSettings;
    const result = applyBootstrapAutoDetect(original, tmpDir);
    expect(original.additionalSettings).toBe(originalSettings);
    expect(original.additionalSettings).toEqual({ foo: "bar" });
    expect(result).not.toBe(original);
    expect(result.additionalSettings).not.toBe(originalSettings);
  });

  it("returns the config unchanged when the bootstrap DLL is absent", () => {
    const config = { appName: "test", additionalSettings: { foo: "bar" } };
    const result = applyBootstrapAutoDetect(config, tmpDir);
    expect(result).toBe(config);
  });
});

if (!isWindows) describe("applyBootstrapAutoDetect (non-Windows)", () => {
  it("is a no-op regardless of files present in the directory", () => {
    const tmp = mkdtempSync(join(tmpdir(), "fl-js-v2-bootstrap-"));
    try {
      writeFileSync(join(tmp, "Microsoft.WindowsAppRuntime.Bootstrap.dll"), "dummy");
      const config = { appName: "test" };
      const result = applyBootstrapAutoDetect(config, tmp);
      expect(result).toBe(config);
    } finally {
      rmSync(tmp, { recursive: true, force: true });
    }
  });
});
