// Tests for configureNativeLoader. These run in their own vitest worker
// process so the addon-load cache is clean — the only assertions exercise
// validation paths that fail BEFORE any pre-load is attempted.
import { describe, expect, it } from "vitest";

import { configureNativeLoader } from "../src/detail/native.js";

describe("configureNativeLoader", () => {
  it("is a no-op when libraryPath is undefined", () => {
    expect(() => configureNativeLoader({})).not.toThrow();
  });

  it("is a no-op when libraryPath is the empty string", () => {
    expect(() => configureNativeLoader({ libraryPath: "" })).not.toThrow();
  });

  it("throws TypeError when the directory does not exist", () => {
    const nonexistent =
      process.platform === "win32"
        ? "C:\\definitely\\does\\not\\exist\\fl-js-sdk-test"
        : "/definitely/does/not/exist/fl-js-sdk-test";
    expect(() => configureNativeLoader({ libraryPath: nonexistent })).toThrow(TypeError);
  });
});
