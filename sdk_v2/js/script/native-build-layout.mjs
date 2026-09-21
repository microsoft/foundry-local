import { existsSync, readFileSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

const scriptDir = fileURLToPath(new URL(".", import.meta.url));

export const cppBuildConfig = process.env.FOUNDRY_LOCAL_CPP_CONFIG ?? "RelWithDebInfo";

const platformSegment = {
  win32: "Windows",
  linux: "Linux",
  darwin: "macOS",
}[process.platform];

if (platformSegment === undefined) {
  throw new Error(`Unsupported platform: ${process.platform}`);
}

const buildDir = resolve(scriptDir, "..", "..", "cpp", "build", platformSegment, cppBuildConfig);

function isMultiConfigBuild() {
  const cachePath = resolve(buildDir, "CMakeCache.txt");
  if (!existsSync(cachePath)) {
    return existsSync(resolve(buildDir, cppBuildConfig));
  }
  return /^CMAKE_CONFIGURATION_TYPES(?::[^=]*)?=.+$/m.test(readFileSync(cachePath, "utf8"));
}

function configuredDir(baseDir) {
  return isMultiConfigBuild() ? resolve(baseDir, cppBuildConfig) : baseDir;
}

export function resolveCppRuntimeBinDir() {
  return configuredDir(resolve(buildDir, "bin"));
}

export function resolveCppImportLibDir() {
  const override = process.env.FOUNDRY_LOCAL_LIB_DIR;
  if (override) return resolve(override);
  return configuredDir(process.platform === "win32" ? buildDir : resolve(buildDir, "bin"));
}
