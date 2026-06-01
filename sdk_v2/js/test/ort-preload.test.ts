// Tests for the ORT preload step inside `configureNativeLoader`. With the dedicated `foundry_local_preload`
// addon in place, `process.dlopen`'s Node 23+ "Module did not self-register" rejection no longer applies —
// the preload addon uses native `LoadLibraryExW` / `dlopen` directly. These tests now run whenever the
// prebuild directory actually contains foundry_local + ORT + GenAI (i.e. after `npm run copy-native:dev`).
import { existsSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { describe, expect, it } from "vitest";

import { configureNativeLoader } from "../src/detail/native.js";

const here = fileURLToPath(new URL(".", import.meta.url));
const pkgRoot = resolve(here, "..");
const prebuildDir = resolve(pkgRoot, "prebuilds", `${process.platform}-${process.arch}`);

function nativeLibBasename(): string {
  if (process.platform === "win32") return "foundry_local.dll";
  if (process.platform === "darwin") return "libfoundry_local.dylib";
  return "libfoundry_local.so";
}

// Skip if the prebuild directory doesn't contain foundry_local (e.g. CI environment where the C++ build
// hasn't been staged). Detecting on disk rather than via an env var keeps the test self-describing.
const haveNativeLib = existsSync(resolve(prebuildDir, nativeLibBasename()));
const describeIfNative = haveNativeLib ? describe : describe.skip;

if (!haveNativeLib) {
  // eslint-disable-next-line no-console
  console.warn(
    `[ort-preload tests] SKIPPED — ${nativeLibBasename()} not found in ${prebuildDir}. Run \`npm run copy-native:dev\` first.`,
  );
}

describeIfNative("configureNativeLoader ORT preload", () => {
  it("loads foundry_local from the dev prebuild directory (ORT preloaded by absolute path)", () => {
    expect(() => configureNativeLoader({ libraryPath: prebuildDir })).not.toThrow();
  });

  it("is idempotent — second call with the same path succeeds (preload flags suppress re-dlopen)", () => {
    expect(() => configureNativeLoader({ libraryPath: prebuildDir })).not.toThrow();
  });
});
