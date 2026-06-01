// Print the directory containing the foundry_local import library / shared lib
// that the addon links against. node-gyp invokes this via `<!(node ...)`.
// Output a single absolute path with no trailing newline.
//
// Overridable via FOUNDRY_LOCAL_LIB_DIR for consumers (CI, downstream
// packagers) that don't have a local sdk_v2/cpp/build tree.
import { existsSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

const override = process.env.FOUNDRY_LOCAL_LIB_DIR;
if (override) {
  if (!existsSync(override)) {
    process.stderr.write(
      `[binding.gyp] FOUNDRY_LOCAL_LIB_DIR points at a missing directory: ${override}\n`,
    );
    process.exit(1);
  }
  process.stdout.write(override);
  process.exit(0);
}

const here = fileURLToPath(new URL(".", import.meta.url));
const repoRoot = resolve(here, "..", "..", "..", "..");

const platformSegment = (() => {
  switch (process.platform) {
    case "win32":
      return "Windows";
    case "linux":
      return "Linux";
    case "darwin":
      return "macOS";
    default:
      return process.platform;
  }
})();

const config = process.env.FOUNDRY_LOCAL_CPP_CONFIG ?? "RelWithDebInfo";
const base = resolve(repoRoot, "sdk_v2", "cpp", "build", platformSegment, config);

// On Windows, MSVC drops the import lib at <build>/<Config>/foundry_local.lib.
// On POSIX, the shared lib lives next to the binaries under bin/<Config>/.
const libDir = process.platform === "win32" ? resolve(base, config) : resolve(base, "bin", config);

if (!existsSync(libDir)) {
  process.stderr.write(
    `[binding.gyp] foundry_local lib directory not found: ${libDir}\n[binding.gyp] Build the C++ SDK first:\n[binding.gyp]   python sdk_v2/cpp/build.py --configure --build --config ${config}\n`,
  );
  process.exit(1);
}

process.stdout.write(libDir);
