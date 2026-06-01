// Singleton enforcement: the underlying `foundry_local::Manager` is a process singleton — the C++ wrapper throws
// if a second one is created. We do NOT duplicate that check in JS; the user-facing failure mode is a tagged
// `FoundryLocalError` from the native addon. `create()` and `createAsync()` are factory wrappers around the
// constructor — the native layer is the source of truth for instance identity.

import { existsSync } from "node:fs";
import { resolve } from "node:path";

import { type Catalog, wrapNativeCatalog } from "./catalog.js";
import { FOUNDRY_LOCAL_CONFIG_KEYS, type FoundryLocalConfig } from "./configuration.js";
import {
  type NativeManager,
  configureNativeLoader,
  getAddon,
  getPreloadedLibraryPath,
  getResolvedLibraryDir,
} from "./detail/native.js";
import type { EpDownloadResult, EpInfo } from "./types.js";

/**
 * On Windows WinML builds, the native side needs the Windows App Runtime bootstrapped (the build co-locates
 * `Microsoft.WindowsAppRuntime.Bootstrap.dll` next to `foundry_local.dll`). Detect that layout at runtime and
 * inject `additionalSettings.Bootstrap = "true"` unless the caller already set it. Returns a shallow-copied
 * config so the caller's `additionalSettings` is never mutated. No-op on non-Windows.
 *
 * Mirrors the `IS_WINML`-gated block in C# `FoundryLocalManager` (sdk_v2/cs/src/FoundryLocalManager.cs), but
 * driven by a filesystem probe instead of a compile-time flag.
 */

export function applyBootstrapAutoDetect(config: FoundryLocalConfig, libraryDir: string): FoundryLocalConfig {
  if (process.platform !== "win32") return config;
  const existing = config.additionalSettings;
  // Defer to native validation when additionalSettings is present but not a plain object — spreading a string,
  // array, or other non-record would silently produce a "valid" object and mask the TypeError the native layer
  // would otherwise throw.
  if (existing !== undefined && (existing === null || typeof existing !== "object" || Array.isArray(existing))) {
    return config;
  }
  if (existing?.Bootstrap !== undefined) return config;
  const bootstrap = resolve(libraryDir, "Microsoft.WindowsAppRuntime.Bootstrap.dll");
  if (!existsSync(bootstrap)) return config;
  return {
    ...config,
    additionalSettings: { ...(existing ?? {}), Bootstrap: "true" },
  };
}

export class FoundryLocalManager {
  readonly #native: NativeManager;
  #catalog: Catalog | undefined;
  #urls: string[] = [];

  constructor(config: FoundryLocalConfig) {
    if (config === null || config === undefined) {
      throw new TypeError("Configuration must be provided.");
    }
    if (typeof config.appName !== "string" || config.appName.trim() === "") {
      throw new TypeError("appName must be set to a valid application name.");
    }
    // Reject typos like `cachePath` (vs. `modelCacheDir`). The native layer would otherwise silently ignore
    // them, sending the user down a confusing debug path (the wrong cache dir, wrong service URL, etc.).
    const unknownKeys = Object.keys(config).filter(
      (key) => !FOUNDRY_LOCAL_CONFIG_KEYS.has(key as keyof FoundryLocalConfig),
    );
    if (unknownKeys.length > 0) {
      const allowed = [...FOUNDRY_LOCAL_CONFIG_KEYS].sort().join(", ");
      throw new TypeError(
        `Unknown FoundryLocalConfig ${unknownKeys.length === 1 ? "property" : "properties"}: ${unknownKeys.join(", ")}. Allowed: ${allowed}.`,
      );
    }
    // Apply libraryPath (if any) before triggering addon load. If the addon
    // is already loaded, only re-applying the same path is a silent no-op;
    // any other path is rejected with a clear message.
    if (config.libraryPath !== undefined && config.libraryPath !== "") {
      const already = getPreloadedLibraryPath();
      if (already !== undefined && already !== config.libraryPath) {
        throw new Error(
          "libraryPath cannot be changed after the native addon has been loaded. Subsequent FoundryLocalManager instances must use the same libraryPath (or omit it).",
        );
      }
      if (already === undefined) {
        try {
          configureNativeLoader({ libraryPath: config.libraryPath });
        } catch (err) {
          if (err instanceof Error) {
            throw new Error(`Failed to configure native loader: ${err.message}`, { cause: err });
          }
          throw err;
        }
      }
    }
    const merged = applyBootstrapAutoDetect(config, getResolvedLibraryDir(config.libraryPath));
    this.#native = new (getAddon().Manager)(merged);
  }

  /**
   * Construct a `FoundryLocalManager`. The native layer is the source of truth for the process singleton —
   * calling `create()` more than once throws a tagged `FoundryLocalError` from the C++ wrapper.
   *
   * The native constructor does no I/O, so there is no difference in blocking behaviour between `create` and
   * `createAsync` other than returning a Promise.
   */
  static create(config: FoundryLocalConfig): FoundryLocalManager {
    return new FoundryLocalManager(config);
  }

  /** Async factory variant. */
  static createAsync(config: FoundryLocalConfig): Promise<FoundryLocalManager> {
    return Promise.resolve(new FoundryLocalManager(config));
  }

  /** The model catalog. Lazily wraps the native handle and is cached. */
  get catalog(): Catalog {
    if (this.#catalog === undefined) {
      this.#catalog = wrapNativeCatalog(this.#native.getCatalog());
    }
    return this.#catalog;
  }

  /** URLs the embedded web service is bound to. Empty when not running. */
  get urls(): string[] {
    return this.#urls;
  }

  /** True when the embedded web service is currently running. */
  get isWebServiceRunning(): boolean {
    return this.#urls.length > 0;
  }

  /** Start the embedded web service. Updates `urls`. */
  startWebService(): void {
    this.#native.startWebService();
    this.#urls = this.#native.getWebServiceEndpoints();
  }

  /** Stop the embedded web service. Clears `urls`. */
  stopWebService(): void {
    if (this.#urls.length === 0) {
      return;
    }
    this.#native.stopWebService();
    this.#urls = [];
  }

  /** Discover available execution providers and their registration status. */
  discoverEps(): EpInfo[] {
    return this.#native.discoverEps();
  }

  /** Download and register execution providers. */
  downloadAndRegisterEps(): Promise<EpDownloadResult>;
  /** Download and register specific execution providers by name. */
  downloadAndRegisterEps(names: string[]): Promise<EpDownloadResult>;
  /** Download and register all EPs, with a progress callback. */
  downloadAndRegisterEps(progressCallback: (epName: string, percent: number) => void): Promise<EpDownloadResult>;
  /** Download and register named EPs, with a progress callback. */
  downloadAndRegisterEps(
    names: string[],
    progressCallback: (epName: string, percent: number) => void,
  ): Promise<EpDownloadResult>;
  async downloadAndRegisterEps(
    namesOrCallback?: string[] | ((epName: string, percent: number) => void),
    progressCallback?: (epName: string, percent: number) => void,
  ): Promise<EpDownloadResult> {
    let names: string[] | undefined;
    let cb: ((epName: string, percent: number) => void) | undefined;
    if (typeof namesOrCallback === "function") {
      cb = namesOrCallback;
    } else if (Array.isArray(namesOrCallback)) {
      names = namesOrCallback;
      cb = progressCallback;
    } else if (progressCallback !== undefined) {
      cb = progressCallback;
    }

    // The native API throws on failure rather than returning a pass/fail result envelope. We capture which EPs
    // the native call asked to register and report them back as `registeredEps` on success; on failure we
    // re-throw the tagged FoundryLocalError unchanged.
    const requested = names ?? [];
    await this.#native.downloadAndRegisterEps(names, cb);

    return {
      success: true,
      status: requested.length > 0 ? "Requested EPs registered" : "All providers registered",
      registeredEps: requested,
      failedEps: [],
    };
  }

  /** Whether an EP download/registration operation is currently in progress. */
  isEpDownloadInProgress(): boolean {
    return this.#native.isEpDownloadInProgress();
  }

  /** Begin graceful shutdown. Idempotent. */
  shutdown(): void {
    this.#native.shutdown();
  }

  /** Whether `shutdown()` has been called. */
  get isShutdownRequested(): boolean {
    return this.#native.isShutdownRequested();
  }

  /** True if `dispose()` has been called on this manager. */
  get disposed(): boolean {
    return this.#native.isDisposed();
  }

  /**
   * Release the underlying native `foundry_local::Manager`. Idempotent. After disposal, every other instance method
   * (and any method on a `Catalog` or `Model` obtained through this manager) throws a `FoundryLocalError`.
   */
  dispose(): void {
    this.#native.dispose();
    this.#catalog = undefined;
    this.#urls = [];
  }

  [Symbol.dispose](): void {
    this.dispose();
  }
}
