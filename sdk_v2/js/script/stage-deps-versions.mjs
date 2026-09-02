import { copyFileSync, existsSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = fileURLToPath(new URL(".", import.meta.url));
const pkgRoot = resolve(here, "..");
const repoRoot = resolve(pkgRoot, "..", "..");
const depsSrc = resolve(repoRoot, "sdk_v2", "deps_versions.json");
const depsDst = resolve(pkgRoot, "deps_versions.json");

if (!existsSync(depsSrc)) {
  throw new Error(`[stage-deps-versions] deps_versions.json not found at ${depsSrc}`);
}

copyFileSync(depsSrc, depsDst);
console.log(`[stage-deps-versions] Staged deps_versions.json -> ${depsDst}`);
