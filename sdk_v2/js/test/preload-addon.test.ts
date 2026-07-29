// Direct test of the `foundry_local_preload` Node-API addon. The addon exposes a single function,
// `preloadLibrary(path: string): void`, that wraps `LoadLibraryExW` on Windows and `dlopen` on POSIX.
// On failure it must throw a JS Error whose message includes both the offending path and a recognisable
// platform error fragment.
import { existsSync } from "node:fs";
import { createRequire } from "node:module";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { beforeAll, describe, expect, it } from "vitest";

const here = fileURLToPath(new URL(".", import.meta.url));
const pkgRoot = resolve(here, "..");
const prebuildDir = resolve(pkgRoot, "prebuilds", `${process.platform}-${process.arch}`);
const preloadAddonPath = resolve(prebuildDir, "foundry_local_preload.node");

const havePreloadAddon = existsSync(preloadAddonPath);

if (!havePreloadAddon) {
  // eslint-disable-next-line no-console
  console.warn(`[preload-addon tests] SKIPPED — ${preloadAddonPath} not found. Run \`npm run build:native\` first.`);
}

interface PreloadAddon {
  preloadLibrary(path: string): void;
}

if (!havePreloadAddon) {
  describe("foundry_local_preload addon", () => {
    it.skip("exports a preloadLibrary function", () => {});
    it.skip("throws a TypeError when called without a string argument", () => {});
    it.skip("throws an Error whose message includes the path and a platform error fragment for a missing library", () => {});
  });
} else {
  describe("foundry_local_preload addon", () => {
    let addon: PreloadAddon;

    beforeAll(() => {
      const require = createRequire(import.meta.url);
      addon = require(preloadAddonPath) as PreloadAddon;
    });

    it("exports a preloadLibrary function", () => {
      expect(typeof addon.preloadLibrary).toBe("function");
    });

    it("throws a TypeError when called without a string argument", () => {
      // biome-ignore lint/suspicious/noExplicitAny: deliberately calling with the wrong shape to exercise validation.
      expect(() => (addon.preloadLibrary as unknown as (...args: any[]) => void)()).toThrow(TypeError);
    });

    it("throws an Error whose message includes the path and a platform error fragment for a missing library", () => {
      const bogusPath = resolve(prebuildDir, "definitely_not_a_real_library_xyz123.bin");
      let caught: unknown;
      try {
        addon.preloadLibrary(bogusPath);
      } catch (err) {
        caught = err;
      }
      expect(caught).toBeInstanceOf(Error);
      const message = (caught as Error).message;
      expect(message).toContain(bogusPath);
      if (process.platform === "win32") {
        // ERROR_MOD_NOT_FOUND (126) or ERROR_FILE_NOT_FOUND (2) — assert on the API name and the parenthesised
        // code suffix the addon appends in `FormatLastError`.
        expect(message).toMatch(/LoadLibraryExW/);
        expect(message).toMatch(/code \d+/);
      } else {
        // glibc / musl / macOS dlerror strings all include "cannot open" or "no such file" — match either.
        expect(message).toMatch(/dlopen|cannot open|no such file/i);
      }
    });
  });
}
