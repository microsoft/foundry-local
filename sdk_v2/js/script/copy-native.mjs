// Dev-time only. Copies foundry_local.{dll,so,dylib} AND its ORT/GenAI/WinML/
// WindowsAppRuntime siblings from sdk_v2/cpp/build/<Platform>/<Config>/bin/
// <Config>/ into sdk_v2/js/prebuilds/<process.platform>-<process.arch>/ so
// `npm test` works locally without the developer having to configure
// Configuration.libraryPath or set env vars.
//
// NOT used by CI publish — see pack-prebuilds.mjs. The published npm tarball
// only ships foundry_local.{dll,so,dylib}; ORT/GenAI/WinML are the user's
// responsibility at runtime per the legacy libraryPath contract.
import { copyFileSync, existsSync, mkdirSync, readdirSync, statSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = fileURLToPath(new URL(".", import.meta.url));
const pkgRoot = resolve(here, "..");
const repoRoot = resolve(pkgRoot, "..", "..");

const config = process.env.FOUNDRY_LOCAL_CPP_CONFIG ?? "RelWithDebInfo";

const platformSegment = (() => {
  switch (process.platform) {
    case "win32":
      return "Windows";
    case "linux":
      return "Linux";
    case "darwin":
      return "macOS";
    default:
      throw new Error(`Unsupported platform: ${process.platform}`);
  }
})();

const sourceDir = resolve(repoRoot, "sdk_v2", "cpp", "build", platformSegment, config, "bin", config);

if (!existsSync(sourceDir)) {
  console.error(`[copy-native] source directory not found: ${sourceDir}`);
  console.error("[copy-native] Build the C++ SDK first:");
  console.error(`[copy-native]   python sdk_v2/cpp/build.py --configure --build --config ${config}`);
  process.exit(1);
}

const destDir = resolve(pkgRoot, "prebuilds", `${process.platform}-${process.arch}`);
mkdirSync(destDir, { recursive: true });

// Files to copy: the foundry_local shared lib + its sibling runtime deps
// (ORT, ORT-GenAI, and on Windows the WinML/AppRuntime bootstraps). The
// addon's process must be able to resolve foundry_local's load-time deps
// via the OS default search (which on Windows includes the addon's own
// directory, on POSIX is satisfied by our rpath of $ORIGIN/@loader_path).
const wanted = (() => {
  if (process.platform === "win32") {
    return [
      "foundry_local.dll",
      "onnxruntime.dll",
      "onnxruntime-genai.dll",
      "onnxruntime_providers_shared.dll",
      "Microsoft.Windows.AI.MachineLearning.dll",
      "Microsoft.WindowsAppRuntime.Bootstrap.dll",
    ];
  }
  if (process.platform === "darwin") {
    return ["libfoundry_local.dylib", "libonnxruntime.dylib", "libonnxruntime-genai.dylib"];
  }
  return ["libfoundry_local.so", "libonnxruntime.so", "libonnxruntime.so.1", "libonnxruntime-genai.so"];
})();

let copied = 0;
const available = new Set(readdirSync(sourceDir));
for (const file of wanted) {
  if (!available.has(file)) {
    // Skip optional deps that aren't present in this build flavor (e.g.
    // WinML bits in a non-WinML build).
    continue;
  }
  const src = resolve(sourceDir, file);
  const dst = resolve(destDir, file);
  copyFileSync(src, dst);
  const size = statSync(dst).size;
  console.log(`[copy-native] ${file} (${size} bytes)`);
  copied += 1;
}

if (copied === 0) {
  console.error(`[copy-native] No expected files found in ${sourceDir}`);
  process.exit(1);
}

console.log(`[copy-native] Copied ${copied} file(s) to ${destDir}`);
