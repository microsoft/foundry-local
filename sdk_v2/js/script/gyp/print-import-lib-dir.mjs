// Print the directory containing the foundry_local import library / shared lib
// that the addon links against. node-gyp invokes this via `<!(node ...)`.
// Output a single absolute path with no trailing newline.
//
// Overridable via FOUNDRY_LOCAL_LIB_DIR for consumers (CI, downstream
// packagers) that don't have a local sdk_v2/cpp/build tree.
import { existsSync } from "node:fs";

import { cppBuildConfig, resolveCppImportLibDir } from "../native-build-layout.mjs";

const override = process.env.FOUNDRY_LOCAL_LIB_DIR;
const libDir = resolveCppImportLibDir();
if (override) {
  if (!existsSync(libDir)) {
    process.stderr.write(`[binding.gyp] FOUNDRY_LOCAL_LIB_DIR points at a missing directory: ${override}\n`);
    process.exit(1);
  }
  process.stdout.write(libDir);
  process.exit(0);
}

if (!existsSync(libDir)) {
  process.stderr.write(
    `[binding.gyp] foundry_local lib directory not found: ${libDir}\n[binding.gyp] Build the C++ SDK first:\n[binding.gyp]   python sdk_v2/cpp/build.py --configure --build --config ${cppBuildConfig}\n`,
  );
  process.exit(1);
}

process.stdout.write(libDir);
