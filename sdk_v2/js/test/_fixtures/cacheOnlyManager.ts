// Shared cache-only Manager fixture for v2 SDK tests.
//
// Mirrors the C++ integration test policy
// (.github/instructions/cpp-testing.instructions.md): one Manager per test
// file, loaded once in `beforeAll` and torn down in `afterAll`. Vitest runs
// each test file in its own worker process, which is what makes this safe
// given the singleton invariant the C++ wrapper enforces
// (`foundry_local::Manager::Create` throws if one already exists in-process).
//
// Cache-only mode mirrors `sdk_v2/cpp/test/sdk_api/cache_only_test.cc`: a
// temp directory containing a hand-rolled `foundry.modelinfo.json`, pointed
// at via `modelCacheDir` + a bogus `externalServiceUrl` so the catalog
// resolves the fixture models without contacting the real backend.
import { existsSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import type { FoundryLocalConfig } from "../../src/configuration.js";
import { FoundryLocalManager } from "../../src/foundryLocalManager.js";

const here = fileURLToPath(new URL(".", import.meta.url));
const pkgRoot = resolve(here, "..", "..");
const prebuildDir = resolve(pkgRoot, "prebuilds", `${process.platform}-${process.arch}`);
const addonPath = resolve(prebuildDir, "foundry_local_node.node");
const nativeLibPath = (() => {
  if (process.platform === "win32") return resolve(prebuildDir, "foundry_local.dll");
  if (process.platform === "darwin") return resolve(prebuildDir, "libfoundry_local.dylib");
  return resolve(prebuildDir, "libfoundry_local.so");
})();

/**
 * True iff the addon and native shared library are both present on disk.
 * Tests use this to gate the suite via `describe.skip`.
 */
export const haveNativePrereqs: boolean = existsSync(addonPath) && existsSync(nativeLibPath);

/**
 * Human-readable diagnostic for the missing-prereqs case. Test files print
 * this once before deciding to skip.
 */
export const nativePrereqsDiagnostic = [
  "[v2 SDK tests] missing dev-build prereqs:",
  `  addon:  ${addonPath} ${existsSync(addonPath) ? "✓" : "✗"}`,
  `  native: ${nativeLibPath} ${existsSync(nativeLibPath) ? "✓" : "✗"}`,
  "Build with:",
  "  npm run copy-native:dev && npm run build:native",
].join("\n");

/** Two-model fixture catalog (phi-4-mini-instruct + qwen2.5-0.5b-instruct). */
const FIXTURE_CATALOG = {
  version: 1,
  savedAtUnix: 1713800000,
  models: [
    {
      id: "phi-4-mini-instruct-generic-cpu:2",
      name: "phi-4-mini-instruct-generic-cpu",
      version: 2,
      alias: "phi-4-mini-instruct",
      uri: "azureml://registries/azureml/models/phi-4-mini-instruct-generic-cpu/versions/2",
      providerType: "AzureFoundry",
      modelType: "ONNX",
      task: "chat-completion",
      publisher: "Microsoft",
      displayName: "Phi-4 Mini Instruct",
    },
    {
      id: "qwen2.5-0.5b-instruct-generic-cpu:1",
      name: "qwen2.5-0.5b-instruct-generic-cpu",
      version: 1,
      alias: "qwen2.5-0.5b-instruct",
      uri: "azureml://registries/azureml/models/qwen2.5-0.5b-instruct-generic-cpu/versions/1",
      providerType: "AzureFoundry",
      modelType: "ONNX",
      task: "chat-completion",
      publisher: "Alibaba",
      displayName: "Qwen 2.5 0.5B Instruct",
    },
  ],
} as const;

export interface CacheOnlyManagerFixture {
  readonly manager: FoundryLocalManager;
  readonly tmpDir: string;
}

export interface SetupOptions {
  /** Override the appName. Defaults to a fixed test id. */
  readonly appName?: string;
}

/**
 * Construct a Manager pointed at a fresh temp cache directory pre-populated
 * with the fixture `foundry.modelinfo.json`. Caller is responsible for
 * passing the returned fixture to {@link teardownCacheOnlyManager}.
 */
export function setupCacheOnlyManager(opts: SetupOptions = {}): CacheOnlyManagerFixture {
  const tmpDir = mkdtempSync(join(tmpdir(), "fl-js-v2-test-"));
  writeFileSync(join(tmpDir, "foundry.modelinfo.json"), JSON.stringify(FIXTURE_CATALOG, null, 2));
  const config: FoundryLocalConfig = {
    appName: opts.appName ?? "foundry-local-js-sdk-v2-tests",
    modelCacheDir: tmpDir,
    serviceEndpoint: "http://127.0.0.1:12345",
  };
  const manager = FoundryLocalManager.create(config);
  return { manager, tmpDir };
}

/** Dispose the Manager (idempotent) and remove the temp directory. */
export function teardownCacheOnlyManager(fixture: CacheOnlyManagerFixture): void {
  if (!fixture.manager.disposed) {
    fixture.manager.dispose();
  }
  rmSync(fixture.tmpDir, { recursive: true, force: true });
}
