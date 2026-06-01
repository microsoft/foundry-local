// Print the absolute prebuilds/<platform>-<arch>/ directory where the
// .node addon is copied after build. node-gyp invokes this via `<!(node ...)`.
//
// Overridable via FOUNDRY_LOCAL_PREBUILD_DIR for cross-compile scenarios
// (e.g. CI building win-arm64 on an x64 host, where process.arch would
// otherwise resolve to the host arch instead of the target arch).
import { mkdirSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

const override = process.env.FOUNDRY_LOCAL_PREBUILD_DIR;
if (override) {
  mkdirSync(override, { recursive: true });
  process.stdout.write(override);
  process.exit(0);
}

const here = fileURLToPath(new URL(".", import.meta.url));
const pkgRoot = resolve(here, "..", "..");
const prebuildDir = resolve(pkgRoot, "prebuilds", `${process.platform}-${process.arch}`);

// node-gyp's copies action requires the destination to exist before we get here.
mkdirSync(prebuildDir, { recursive: true });

process.stdout.write(prebuildDir);
