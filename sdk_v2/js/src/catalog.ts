// `Catalog` instances are NEVER constructed directly by user code — they are returned from
// `FoundryLocalManager.catalog`. The underlying native catalog is owned by the parent manager; the JS wrapper
// holds a reference to keep the manager alive while the catalog is reachable.
//
// The underlying native catalog operations are synchronous; the async surface here is for parity with the C# /
// Python SDKs. `getModel`, `getModelVariant`, and `getLatestVersion` throw when the alias / id is not found.

import type { NativeCatalog, NativeModel } from "./detail/native.js";
import type { IModel } from "./imodel.js";
import { Model, unwrapNativeModel, wrapNativeModel } from "./model.js";
import { type MutableModelInfo, unwrapMutableModelInfo } from "./modelInfo.js";

const internalCtorKey = Symbol("Catalog.internal");
const nativeByCatalog = new WeakMap<Catalog, NativeCatalog>();

export class Catalog {
  readonly #native: NativeCatalog;

  /** @internal — wraps a native catalog handle. Do not call from user code. */
  constructor(token: typeof internalCtorKey, native: NativeCatalog) {
    if (token !== internalCtorKey) {
      throw new TypeError("Catalog is internal — obtain instances via FoundryLocalManager.catalog");
    }
    this.#native = native;
    nativeByCatalog.set(this, native);
  }

  /** Catalog name (e.g. `"AzureFoundryCatalog"`). */
  get name(): string {
    return this.#native.getName();
  }

  /** All models in the catalog. */
  async getModels(): Promise<IModel[]> {
    return wrapAll(this.#native.getModels());
  }

  /** Every individual model variant currently present in the local cache. */
  async getCachedModels(): Promise<IModel[]> {
    return wrapAll(this.#native.getCachedModels());
  }

  /** Models currently loaded into memory. */
  async getLoadedModels(): Promise<IModel[]> {
    return wrapAll(this.#native.getLoadedModels());
  }

  /**
   * Look up a model by alias. Throws when the alias is not present in the catalog. Callers that want a
   * "find or undefined" shape should iterate `getModels()` themselves.
   */
  async getModel(alias: string): Promise<IModel> {
    validateNonEmptyString(alias, "Model alias");
    const n = this.#native.getModel(alias);
    if (n === undefined) {
      throw new Error(`Model with alias '${alias}' not found.`);
    }
    return wrapNativeModel(n);
  }

  /** Look up a specific model variant by its full model id. Throws when not found. */
  async getModelVariant(modelId: string): Promise<IModel> {
    validateNonEmptyString(modelId, "Model ID");
    const n = this.#native.getModelVariant(modelId);
    if (n === undefined) {
      throw new Error(`Model variant with ID '${modelId}' not found.`);
    }
    return wrapNativeModel(n);
  }

  /** Resolve the latest catalog version for the same model name. Throws when not found. */
  async getLatestVersion(model: IModel): Promise<IModel> {
    if (!(model instanceof Model)) {
      throw new TypeError("Catalog.getLatestVersion: expected a Model instance");
    }
    const n = this.#native.getLatestVersion(unwrapNativeModel(model));
    if (n === undefined) {
      throw new Error(`Latest version for model '${model.alias}' not found.`);
    }
    return wrapNativeModel(n);
  }

  /**
   * Get all versions of a model alias, optionally narrowed to a single variant name. `maxVersions`
   * defaults to 50 and acts as a per-variant cap; pass 0 or a negative value for no cap.
   */
  async getModelVersions(modelAlias: string, modelName?: string, maxVersions = 50): Promise<IModel[]> {
    validateNonEmptyString(modelAlias, "Model alias");
    if (modelName !== undefined) {
      validateNativeString(modelName, "Model name");
    }
    // The native parameter is int32_t; N-API's Int32Value() silently coerces
    // fractions, NaN, Infinity, and out-of-range values into a different cap.
    // Reject them here (matching Rust's i32::MAX rejection) so callers get a
    // clear error rather than a surprising truncated result. Negatives/0 are
    // valid ("no cap") but must still fit i32.
    if (!Number.isInteger(maxVersions) || maxVersions < -(2 ** 31) || maxVersions > 2 ** 31 - 1) {
      throw new TypeError("maxVersions must be an integer within the 32-bit range.");
    }
    return wrapAll(await this.#native.getModelVersions(modelAlias, modelName ?? null, maxVersions));
  }

  /**
   * Register existing local model assets. Use this only with `CatalogType.Local`; public catalogs reject mutation.
   * Registration requires `task`. Display name, publisher, validated runtime metadata, modalities, and custom
   * properties are preserved and may receive authoritative defaults. Identity, alias, type, timestamps, context length,
   * and prompt templates are SDK-derived; caller location and internal metadata are ignored. A caller-supplied provider
   * becomes the default load override; an SDK-derived artifact provider leaves `genai_config.json` options intact. The
   * catalog never deletes `modelPath`.
   */
  async registerModel(modelPath: string, modelId: string, metadata: MutableModelInfo): Promise<IModel> {
    validateRegistrationArgs(modelPath, modelId, metadata);
    return wrapNativeModel(await this.#native.registerModel(modelPath, modelId, unwrapMutableModelInfo(metadata)));
  }

  /**
   * Synchronous registration variant with the same metadata ownership rules as `registerModel()`. This performs file
   * I/O and blocks the event loop; prefer `registerModel()`.
   */
  registerModelSync(modelPath: string, modelId: string, metadata: MutableModelInfo): IModel {
    validateRegistrationArgs(modelPath, modelId, metadata);
    return wrapNativeModel(this.#native.registerModelSync(modelPath, modelId, unwrapMutableModelInfo(metadata)));
  }

  /** Unregister a local model without deleting its assets. Accepts either an alias or a full model ID. */
  async unregisterModel(aliasOrModelId: string): Promise<void> {
    validateNonEmptyString(aliasOrModelId, "Alias or model ID");
    await this.#native.unregisterModel(aliasOrModelId);
  }

  /** Synchronous unregistration variant. This performs file I/O and blocks the event loop. */
  unregisterModelSync(aliasOrModelId: string): void {
    validateNonEmptyString(aliasOrModelId, "Alias or model ID");
    this.#native.unregisterModelSync(aliasOrModelId);
  }
}

/** @internal — used by `FoundryLocalManager` to wrap a native catalog. */
export function wrapNativeCatalog(native: NativeCatalog): Catalog {
  return new Catalog(internalCtorKey, native);
}

/** @internal Test-only access for exercising native async lifetime boundaries. */
export function unwrapNativeCatalog(catalog: Catalog): NativeCatalog {
  const native = nativeByCatalog.get(catalog);
  if (native === undefined) {
    throw new TypeError("Expected a Catalog instance");
  }
  return native;
}

function wrapAll(natives: readonly NativeModel[]): IModel[] {
  return natives.map((n) => wrapNativeModel(n));
}

function validateNonEmptyString(value: string, name: string): void {
  if (typeof value !== "string" || value.trim() === "") {
    throw new TypeError(`${name} must be a non-empty string.`);
  }
  validateNativeString(value, name);
}

function validateNativeString(value: string, name: string): void {
  if (value.includes("\0")) {
    throw new TypeError(`${name} must not contain an embedded NUL character.`);
  }
}

function validateRegistrationArgs(modelPath: string, modelId: string, metadata: MutableModelInfo): void {
  validateNonEmptyString(modelPath, "Model path");
  validateNonEmptyString(modelId, "Model ID");
  unwrapMutableModelInfo(metadata);
}
