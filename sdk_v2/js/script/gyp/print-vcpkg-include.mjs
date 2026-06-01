// Print the vcpkg-installed include directory used by the C++ SDK build.
// node-gyp invokes this at configure time via `<!(node ...)` in binding.gyp.
// Output a single absolute path with no trailing newline.
//
// Overridable via FOUNDRY_LOCAL_INCLUDE_DIR for consumers (CI, downstream
// packagers) that don't have a local sdk_v2/cpp/build tree.
import { existsSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

const override = process.env.FOUNDRY_LOCAL_INCLUDE_DIR;
if (override) {
  if (!existsSync(override)) {
    process.stderr.write(
      `[binding.gyp] FOUNDRY_LOCAL_INCLUDE_DIR points at a missing directory: ${override}\n`,
    );
    process.exit(1);
  }
  process.stdout.write(override);
  process.exit(0);
}

const here = fileURLToPath(new URL(".", import.meta.url));
const repoRoot = resolve(here, "..", "..", "..", "..");

// Map Node's process.platform -> the segment build.py uses for the canonical
// build dir. Mirrors script/copy-native.mjs and .github/instructions/cpp-build.
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

const triplet = (() => {
  switch (process.platform) {
    case "win32":
      return process.arch === "arm64" ? "arm64-windows" : "x64-windows";
    case "linux":
      return process.arch === "arm64" ? "arm64-linux" : "x64-linux";
    case "darwin":
      return process.arch === "arm64" ? "arm64-osx" : "x64-osx";
    default:
      return "x64-linux";
  }
})();

const config = process.env.FOUNDRY_LOCAL_CPP_CONFIG ?? "RelWithDebInfo";

const candidate = resolve(
  repoRoot,
  "sdk_v2",
  "cpp",
  "build",
  platformSegment,
  config,
  "vcpkg_installed",
  triplet,
  "include",
);

if (!existsSync(candidate)) {
  // Fail loud — better than silently producing an empty include path that
  // makes #include <gsl/span> fail with a confusing error.
  process.stderr.write(
    `[binding.gyp] vcpkg include dir not found: ${candidate}\n[binding.gyp] Build the C++ SDK first:\n[binding.gyp]   python sdk_v2/cpp/build.py --configure --build --config ${config}\n`,
  );
  process.exit(1);
}

process.stdout.write(candidate);
