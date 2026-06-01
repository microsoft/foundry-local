// `Catalog` instances are NEVER constructed directly by user code — they are returned from
// `FoundryLocalManager.catalog`. The underlying native catalog is owned by the parent manager; the JS wrapper
// holds a reference to keep the manager alive while the catalog is reachable.
//
// The underlying native catalog operations are synchronous; the async surface here is for parity with the C# /
// Python SDKs. `getModel`, `getModelVariant`, and `getLatestVersion` throw when the alias / id is not found.

import type { NativeCatalog, NativeModel } from "./detail/native.js";
import type { IModel } from "./imodel.js";
import { Model, unwrapNativeModel, wrapNativeModel } from "./model.js";

const internalCtorKey = Symbol("Catalog.internal");

export class Catalog {
  readonly #native: NativeCatalog;

  /** @internal — wraps a native catalog handle. Do not call from user code. */
  constructor(token: typeof internalCtorKey, native: NativeCatalog) {
    if (token !== internalCtorKey) {
      throw new TypeError("Catalog is internal — obtain instances via FoundryLocalManager.catalog");
    }
    this.#native = native;
  }

  /** Catalog name (e.g. `"AzureFoundryCatalog"`). */
  get name(): string {
    return this.#native.getName();
  }

  /** All models in the catalog. */
  async getModels(): Promise<IModel[]> {
    return wrapAll(this.#native.getModels());
  }

  /** Models currently present in the local cache. */
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
    if (typeof alias !== "string" || alias.trim() === "") {
      throw new Error("Model alias must be a non-empty string.");
    }
    const n = this.#native.getModel(alias);
    if (n === undefined) {
      throw new Error(`Model with alias '${alias}' not found.`);
    }
    return wrapNativeModel(n);
  }

  /** Look up a specific model variant by its full model id. Throws when not found. */
  async getModelVariant(modelId: string): Promise<IModel> {
    if (typeof modelId !== "string" || modelId.trim() === "") {
      throw new Error("Model ID must be a non-empty string.");
    }
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
}

/** @internal — used by `FoundryLocalManager` to wrap a native catalog. */
export function wrapNativeCatalog(native: NativeCatalog): Catalog {
  return new Catalog(internalCtorKey, native);
}

function wrapAll(natives: readonly NativeModel[]): IModel[] {
  return natives.map((n) => wrapNativeModel(n));
}
