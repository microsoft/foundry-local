// Loads the native addon from prebuilds/<platform>-<arch>/foundry_local_node.node.
// In published packages this directory is populated by CI; in dev it is
// populated by `npm run build:native` (output target) and the C++ shared lib
// is dropped alongside by `npm run copy-native:dev`.
import { existsSync, statSync } from "node:fs";
import { createRequire } from "node:module";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

// JSON-stringifiable shape of the addon's `Manager` class. The native side
// uses `Napi::ObjectWrap<Manager>` and exports a JS constructor.
export interface NativeManagerCtor {
  new (options: {
    appName: string;
    modelCacheDir?: string;
    serviceEndpoint?: string;
    appDataDir?: string;
    logsDir?: string;
    logLevel?: "trace" | "debug" | "info" | "warn" | "error" | "fatal";
    webServiceUrls?: string;
    additionalSettings?: { [key: string]: string };
  }): NativeManager;
}

export interface NativeManager {
  getWebServiceEndpoints(): string[];
  getCatalog(): NativeCatalog;
  startWebService(): void;
  stopWebService(): void;
  discoverEps(): Array<{ name: string; isRegistered: boolean }>;
  downloadAndRegisterEps(names?: string[], onProgress?: (epName: string, percent: number) => void): Promise<void>;
  isEpDownloadInProgress(): boolean;
  shutdown(): void;
  isShutdownRequested(): boolean;
  dispose(): void;
  isDisposed(): boolean;
}

// Raw snapshot returned by the native side. All optional fields are omitted
// (rather than serialised as `null`) when the wrapper reports them as unset.
// `deviceType` is already mapped to the public string union on the native side.
export interface NativeModelInfo {
  id: string;
  name: string;
  version: number;
  alias: string;
  uri: string;
  deviceType: "CPU" | "GPU" | "NPU" | "Invalid";
  providerType?: string;
  executionProvider?: string;
  displayName?: string;
  modelType?: string;
  promptTemplate?: {
    system?: string;
    user?: string;
    assistant?: string;
    prompt?: string;
  };
  publisher?: string;
  modelSettings?: {
    parameters?: Array<{ name: string; value?: string | null }>;
  };
  license?: string;
  licenseDescription?: string;
  cached?: boolean;
  task?: string;
  runtime?: {
    deviceType: "CPU" | "GPU" | "NPU" | "Invalid";
    executionProvider: string;
  };
  modelProvider?: string;
  minFLVersion?: string;
  parentUri?: string;
  supportsToolCalling?: boolean;
  fileSizeMb?: number;
  maxOutputTokens?: number;
  createdAtUnix: number;
  isTestModel: boolean;
  contextLength?: number;
  inputModalities?: string;
  outputModalities?: string;
  capabilities?: string;
}

export interface NativeModel {
  getInfo(): NativeModelInfo;
  isCached(): boolean;
  isLoaded(): boolean;
  getPath(): string;
  getVariants(): NativeModel[];
  selectVariant(variant: NativeModel): void;
  load(): Promise<void>;
  unload(): Promise<void>;
  download(progress?: (percent: number) => void): Promise<void>;
  removeFromCache(): void;
}

export interface NativeCatalog {
  getName(): string;
  getModels(): NativeModel[];
  getCachedModels(): NativeModel[];
  getLoadedModels(): NativeModel[];
  getModel(alias: string): NativeModel | undefined;
  getModelVariant(modelId: string): NativeModel | undefined;
  getLatestVersion(model: NativeModel): NativeModel | undefined;
}

// ── Inference surface ───────────────────────────────────────────────────────

/**
 * Plain-object snapshot of an inference response. The native layer fully
 * materialises this on the JS thread before resolving the Promise — there is
 * no native handle behind it.
 */
export interface NativeResponse {
  output: ReadonlyArray<unknown>; // `Item[]` once narrowed via the public surface
  finishReason: "none" | "stop" | "length" | "toolCalls" | "error";
  usage: { promptTokens: number; completionTokens: number; totalTokens: number };
}

export interface NativeRequestCtor {
  new (): NativeRequest;
}

/**
 * Structural mirror of the public `RequestOptions` shape. Duplicated here
 * (rather than imported from `../request.js`) to keep `detail/native.ts`
 * dependency-free of the public surface.
 */
export interface NativeRequestOptions {
  search?: {
    temperature?: number;
    topP?: number;
    topK?: number;
    maxOutputTokens?: number;
    frequencyPenalty?: number;
    presencePenalty?: number;
    seed?: number;
    earlyStopping?: boolean;
    doSample?: boolean;
  };
  toolChoice?: "auto" | "none" | "required";
  additionalOptions?: Readonly<Record<string, string | number | boolean | undefined>>;
}

export interface NativeRequest {
  addItem(item: unknown): void;
  setOptions(options: NativeRequestOptions): void;
  cancel(): void;
  getItemCount(): number;
  getItem(index: number): unknown;
}

export interface NativeItemQueueCtor {
  new (): NativeItemQueue;
}

export interface NativeItemQueue {
  push(item: unknown): void;
  tryPop(): unknown;
  size(): number;
  markFinished(): void;
  finished(): boolean;
  dispose(): void;
}

export interface NativeSession {
  processRequest(request: NativeRequest): Promise<NativeResponse>;
  processStreamingRequest(request: NativeRequest, onItem: (item: unknown) => void): Promise<NativeResponse>;
  setOptions(options: NativeRequestOptions): void;
  dispose(): void;
  isDisposed(): boolean;
}

export interface NativeChatSession extends NativeSession {
  addToolDefinition(definition: { name: string; description: string; jsonSchema: string }): void;
  removeToolDefinition(name: string): boolean;
  turnCount(): number;
  undoTurns(count: number): void;
}

// Embeddings sessions expose no methods beyond the base. The type alias
// exists so the addon export can be typed distinctly from chat.
export type NativeEmbeddingsSession = NativeSession;

// Audio sessions also expose no methods beyond the base — all inference
// (one-shot and streaming) flows through the inherited surface. Audio IS
// streamable (live transcription), unlike embeddings, but that's a
// behavioural difference at the native registration site, not a TS-shape
// difference.
export type NativeAudioSession = NativeSession;

export interface NativeAddon {
  Manager: NativeManagerCtor;
  // `Catalog` / `Model` constructors are exported for `instanceof` checks
  // only; direct construction (`new addon.X()`) throws.
  // Typed as `new (...) => unknown` rather than `Function` to satisfy Biome —
  // we never call these constructors from TS, we only read them.
  Catalog: new (
    ...args: unknown[]
  ) => unknown;
  Model: new (...args: unknown[]) => unknown;
  // `Request` IS directly constructible from JS — it's a stateful builder.
  Request: NativeRequestCtor;
  // `ItemQueue` is directly constructible — the public TS `ItemQueue` class
  // wraps it. The only `Item` subtype with a real native handle on the JS
  // side; all the others are plain JS objects copied across the boundary.
  ItemQueue: NativeItemQueueCtor;
  // Only modality-specific session classes (`ChatSession` today;
  // `AudioSession` / `EmbeddingsSession` later) are directly constructible
  // from JS. They take a JS Model instance (the native ObjectWrap<Model>,
  // i.e. the value stored in the `nativeByModel` weak map) and produce a
  // native session handle. The abstract base `Session` is a TS-only class
  // and has no JS-constructible native counterpart.
  ChatSession: new (
    model: NativeModel,
  ) => NativeChatSession;
  EmbeddingsSession: new (model: NativeModel) => NativeEmbeddingsSession;
  AudioSession: new (model: NativeModel) => NativeAudioSession;
}

const here = fileURLToPath(new URL(".", import.meta.url));
// dist/detail/native.js -> dist/ -> package root
const pkgRoot = resolve(here, "..", "..");
const prebuildDir = resolve(pkgRoot, "prebuilds", `${process.platform}-${process.arch}`);
const addonPath = resolve(prebuildDir, "foundry_local_node.node");
const preloadAddonPath = resolve(prebuildDir, "foundry_local_preload.node");

interface PreloadAddon {
  preloadLibrary(path: string): void;
}

let preloadAddon: PreloadAddon | undefined;

/**
 * Lazily load the tiny `foundry_local_preload` Node-API addon. It exists solely to expose a native
 * `LoadLibraryExW` / `dlopen` entry point for arbitrary shared libraries — Node 23+ rejects `process.dlopen`
 * for anything that isn't itself a Node-API addon, and our `foundry_local.{dll,so,dylib}` plus ORT/GenAI are
 * plain C++ libraries. The preload addon has zero link dependencies on foundry_local or its deps, so it can
 * safely load before they are resident.
 */
function getPreloadAddon(): PreloadAddon {
  if (preloadAddon === undefined) {
    if (!existsSync(preloadAddonPath)) {
      throw new Error(
        `Native preload addon not found at ${preloadAddonPath}.\nBuild it locally with:\n  npm run build:native\n(requires the C++ SDK to be built first via\n \`python sdk_v2/cpp/build.py --configure --build --config RelWithDebInfo\`).`,
      );
    }
    const require = createRequire(import.meta.url);
    preloadAddon = require(preloadAddonPath) as PreloadAddon;
  }
  return preloadAddon;
}

function loadAddon(): NativeAddon {
  if (!existsSync(addonPath)) {
    throw new Error(
      `Native addon not found at ${addonPath}.\nBuild it locally with:\n  npm run copy-native:dev && npm run build:native\n(requires the C++ SDK to be built first via\n \`python sdk_v2/cpp/build.py --configure --build --config RelWithDebInfo\`).\nAlternatively, pass \`libraryPath\` in the FoundryLocalConfig (or call \`configureNativeLoader\`) to point at a directory containing the native library.`,
    );
  }
  const require = createRequire(import.meta.url);
  return require(addonPath) as NativeAddon;
}

let cached: NativeAddon | undefined;

export function getAddon(): NativeAddon {
  if (cached === undefined) {
    cached = loadAddon();
  }
  return cached;
}

let preloaded: string | undefined;
let ortPreloaded = false;
let genAiPreloaded = false;

/** The basename of the native foundry_local shared library on the current platform. */
function nativeLibBasename(): string {
  if (process.platform === "win32") return "foundry_local.dll";
  if (process.platform === "darwin") return "libfoundry_local.dylib";
  return "libfoundry_local.so";
}

/**
 * Candidate basenames for an ORT-family library on the current platform. The loader tries them in order and
 * uses the first that exists on disk. Linux ORT ships as `libonnxruntime.so.1` in some packaging variants
 * (CMake symlink missing); the `.so.1` fallback covers that case. GenAI does not have a `.so.1` variant.
 */
function ortCandidateBasenames(name: "onnxruntime" | "onnxruntime-genai"): string[] {
  if (process.platform === "win32") return [`${name}.dll`];
  if (process.platform === "darwin") return [`lib${name}.dylib`];
  if (name === "onnxruntime") return ["libonnxruntime.so", "libonnxruntime.so.1"];
  return [`lib${name}.so`];
}

/**
 * Pre-load ORT and ORT-GenAI from `directory` by absolute path so that when foundry_local is loaded, the OS
 * loader resolves its NEEDED entries against the already-resident modules instead of doing a filesystem search
 * (RPATH, PATH, LD_LIBRARY_PATH, etc.). Mirrors C# `DllLoader.PreloadOrtIfPresent`.
 *
 * Order matters: ORT before GenAI (GenAI's NEEDED entry references ORT). Missing files are silently skipped —
 * the subsequent foundry_local load will surface a clearer error. dlopen failures are logged but not thrown for
 * the same reason: let the cascading foundry_local error be the authoritative one the user sees.
 *
 * Idempotent across repeated calls via module-scope flags. The process holds the dlopen handle, so we don't
 * need to keep the shim object alive.
 */
function preloadOrtIfPresent(directory: string): void {
  const tryPreload = (name: "onnxruntime" | "onnxruntime-genai"): boolean => {
    for (const basename of ortCandidateBasenames(name)) {
      const fullPath = resolve(directory, basename);
      if (!existsSync(fullPath)) continue;
      try {
        getPreloadAddon().preloadLibrary(fullPath);
        return true;
      } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        // eslint-disable-next-line no-console
        console.error(`[foundry-local] failed to preload ${name} from ${fullPath}: ${message}`);
        // Try the next candidate (or fall through to silent skip).
      }
    }
    return false;
  };

  if (!ortPreloaded && tryPreload("onnxruntime")) ortPreloaded = true;
  if (!genAiPreloaded && tryPreload("onnxruntime-genai")) genAiPreloaded = true;
}

/**
 * Pre-load the native Foundry Local shared library from a specific directory.
 *
 * This must be called *before* the native addon is loaded — i.e. before the first `FoundryLocalManager`
 * construction. Once the addon is loaded, the resolved library is fixed for the lifetime of the process.
 *
 * The `FoundryLocalManager` constructor calls this automatically when `FoundryLocalConfig.libraryPath` is set;
 * advanced callers can invoke it directly to pin the location earlier (e.g. before any lazy import of the SDK
 * resolves the addon).
 */
export function configureNativeLoader(opts: { libraryPath?: string }): void {
  const libraryPath = opts.libraryPath;
  if (libraryPath === undefined || libraryPath === "") {
    return;
  }
  if (cached !== undefined) {
    throw new Error(
      "configureNativeLoader must be called before the native addon is loaded (first FoundryLocalManager construction).",
    );
  }
  if (!existsSync(libraryPath) || !statSync(libraryPath).isDirectory()) {
    throw new TypeError(`libraryPath is not a directory: ${libraryPath}`);
  }
  const expected = nativeLibBasename();
  const fullPath = resolve(libraryPath, expected);
  if (!existsSync(fullPath)) {
    throw new Error(`libraryPath does not contain ${expected}: ${libraryPath}`);
  }

  // On Windows, prepending to PATH biases LoadLibraryEx's default search so transitive DLL dependencies in
  // `libraryPath` resolve as well (belt-and-suspenders for deps we didn't explicitly preload). On POSIX,
  // the preload addon uses RTLD_GLOBAL so the library's symbols go in the global namespace and the addon's
  // NEEDED entry binds to them.
  if (process.platform === "win32") {
    const current = process.env.PATH ?? "";
    if (!current.split(";").some((p) => p === libraryPath)) {
      process.env.PATH = `${libraryPath};${current}`;
    }
  }

  // Preload ORT then ORT-GenAI by absolute path before foundry_local. The OS loader processes foundry_local's
  // NEEDED entries at load time; if ORT isn't already resident the load fails. See the ORT loading contract
  // (.github/instructions/ort-loading-contract.instructions.md) — every binding must do this.
  preloadOrtIfPresent(libraryPath);

  try {
    getPreloadAddon().preloadLibrary(fullPath);
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    throw new Error(`Failed to pre-load native library at ${fullPath}: ${message}`);
  }

  preloaded = libraryPath;
}

/**
 * Returns the directory that was passed to `configureNativeLoader` (or inferred from the first manager's
 * `libraryPath`), or `undefined` if no explicit path has been applied.
 */
export function getPreloadedLibraryPath(): string | undefined {
  return preloaded;
}

/**
 * Returns the directory the native foundry_local shared library is resolved from for the given config —
 * either the caller's explicit `libraryPath` or the prebuild directory the addon itself lives in (which is
 * where `copy-native:dev` and CI prebuild populate `foundry_local.{dll,so,dylib}`).
 */
export function getResolvedLibraryDir(libraryPath?: string): string {
  if (libraryPath !== undefined && libraryPath !== "") return libraryPath;
  return prebuildDir;
}
