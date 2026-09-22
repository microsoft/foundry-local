// `Model` wraps the native model handle directly. There is no JS-level Model/Variant split — the native catalog
// already returns the resolved variant.
//
// `Model` instances are NEVER constructed directly by user code — they come from `Catalog` methods or from
// `model.variants`. The underlying native handle is pinned to its parent `FoundryLocalManager`; once the manager
// is disposed, any further calls reject with a `FoundryLocalError`.

import type { NativeModel, NativeModelInfo } from "./detail/native.js";
import type { IModel } from "./imodel.js";
import { AudioClient } from "./openai/audioClient.js";
import { ChatClient } from "./openai/chatClient.js";
import { EmbeddingClient } from "./openai/embeddingClient.js";
import { DeviceType, type ModelInfo, type ModelSettings, type Parameter, type PromptTemplate } from "./types.js";

const internalCtorKey = Symbol("Model.internal");

const nativeByModel = new WeakMap<Model, NativeModel>();

function isAbortSignal(value: unknown): value is AbortSignal {
  return (
    typeof value === "object" &&
    value !== null &&
    typeof (value as AbortSignal).aborted === "boolean" &&
    typeof (value as AbortSignal).addEventListener === "function" &&
    typeof (value as AbortSignal).removeEventListener === "function"
  );
}

function makeAbortError(message: string): Error {
  const error = new Error(message);
  error.name = "AbortError";
  return error;
}

function toDeviceType(value: NativeModelInfo["deviceType"]): DeviceType {
  switch (value) {
    case "CPU":
      return DeviceType.CPU;
    case "GPU":
      return DeviceType.GPU;
    case "NPU":
      return DeviceType.NPU;
    default:
      return DeviceType.Invalid;
  }
}

function normalizePromptTemplate(raw: NativeModelInfo["promptTemplate"]): PromptTemplate | null {
  if (raw === undefined) {
    return null;
  }
  const assistant = raw.assistant;
  const prompt = raw.prompt;
  if (assistant === undefined || prompt === undefined) {
    return null;
  }
  return {
    assistant,
    prompt,
    system: raw.system ?? null,
    user: raw.user ?? null,
  };
}

function normalizeModelSettings(raw: NativeModelInfo["modelSettings"]): ModelSettings | null {
  if (raw === undefined || raw.parameters === undefined) {
    return null;
  }
  const parameters: Parameter[] = raw.parameters.map((p) => ({
    name: p.name,
    value: p.value ?? null,
  }));
  return { parameters };
}

function normalizeModelInfo(raw: NativeModelInfo, native: NativeModel): ModelInfo {
  const deviceType = toDeviceType(raw.deviceType);
  const executionProvider = raw.executionProvider ?? "";
  const snapshot = {
    ...raw,
    deviceType,
    providerType: raw.providerType ?? raw.modelProvider ?? "",
    // PromptTemplate remains in the contract for back-compat but is deprecated.
    promptTemplate: normalizePromptTemplate(raw.promptTemplate),
    modelSettings: normalizeModelSettings(raw.modelSettings),
    cached: raw.cached ?? native.isCached(),
    runtime:
      raw.runtime !== undefined
        ? {
            deviceType: toDeviceType(raw.runtime.deviceType),
            executionProvider: raw.runtime.executionProvider,
          }
        : {
            deviceType,
            executionProvider,
          },
  };
  Object.defineProperties(snapshot, {
    getStringProperty: {
      enumerable: false,
      value: (key: string) => {
        validateNativeString(key, "ModelInfo property key");
        return native.getStringProperty(key);
      },
    },
    getIntProperty: {
      enumerable: false,
      value: (key: string, defaultValue = 0) => {
        validateNativeString(key, "ModelInfo property key");
        if (typeof defaultValue !== "number") {
          throw new TypeError("ModelInfo integer property default must be a number.");
        }
        if (!Number.isSafeInteger(defaultValue)) {
          throw new RangeError("ModelInfo integer property default must be a safe integer.");
        }
        return native.getIntProperty(key, defaultValue);
      },
    },
  });
  return snapshot as ModelInfo;
}

function validateNativeString(value: string, argumentName: string): void {
  if (typeof value !== "string") {
    throw new TypeError(`${argumentName} must be a string.`);
  }
  if (value.includes("\0")) {
    throw new TypeError(`${argumentName} must not contain an embedded NUL character.`);
  }
}

export class Model implements IModel {
  readonly #native: NativeModel;

  /** @internal — wraps a native Model handle. Do not call from user code. */
  constructor(token: typeof internalCtorKey, native: NativeModel) {
    if (token !== internalCtorKey) {
      throw new TypeError("Model is internal — obtain instances via Catalog/FoundryLocalManager methods");
    }
    this.#native = native;
    nativeByModel.set(this, native);
  }

  get id(): string {
    return this.info.id;
  }

  get alias(): string {
    return this.info.alias;
  }

  get info(): ModelInfo {
    // The native model is the source of truth. Read fresh every time so metadata stays correct after
    // selectVariant / download / cache changes. Each read returns a point-in-time snapshot.
    return normalizeModelInfo(this.#native.getInfo(), this.#native);
  }

  getStringProperty(key: string): string | undefined {
    return this.info.getStringProperty(key);
  }

  getIntProperty(key: string, defaultValue = 0): number {
    return this.info.getIntProperty(key, defaultValue);
  }

  get isCached(): boolean {
    return this.#native.isCached();
  }

  /**
   * Async accessor. The underlying native check is synchronous, but the API is `Promise<boolean>` for parity
   * with the C# / Python SDKs whose "is loaded" check actually performs a backend round-trip.
   */
  async isLoaded(): Promise<boolean> {
    return this.#native.isLoaded();
  }

  get path(): string {
    return this.#native.getPath();
  }

  get variants(): IModel[] {
    const arr = this.#native.getVariants();
    return arr.map((n) => new Model(internalCtorKey, n));
  }

  get contextLength(): number | null {
    return this.info.contextLength ?? null;
  }

  get inputModalities(): string | null {
    return this.info.inputModalities ?? null;
  }

  get outputModalities(): string | null {
    return this.info.outputModalities ?? null;
  }

  get capabilities(): string | null {
    return this.info.capabilities ?? null;
  }

  get supportsToolCalling(): boolean | null {
    return this.info.supportsToolCalling ?? null;
  }

  async load(): Promise<void> {
    await this.#native.load();
  }

  async unload(): Promise<void> {
    await this.#native.unload();
  }

  async download(signal?: AbortSignal): Promise<void>;
  async download(progressCallback?: (progress: number) => void, signal?: AbortSignal): Promise<void>;
  async download(
    progressCallbackOrSignal?: ((progress: number) => void) | AbortSignal,
    optionalSignal?: AbortSignal,
  ): Promise<void> {
    const signal = isAbortSignal(progressCallbackOrSignal) ? progressCallbackOrSignal : optionalSignal;
    const progressCallback = typeof progressCallbackOrSignal === "function" ? progressCallbackOrSignal : undefined;

    if (
      progressCallbackOrSignal !== undefined &&
      progressCallback === undefined &&
      !isAbortSignal(progressCallbackOrSignal)
    ) {
      throw new TypeError("Model.download: first argument must be a progress callback or AbortSignal");
    }
    if (signal !== undefined && !isAbortSignal(signal)) {
      throw new TypeError("Model.download: second argument must be an AbortSignal");
    }
    if (signal?.aborted === true) {
      throw makeAbortError("Model download aborted before start");
    }

    await this.#native.download(progressCallback, signal);
  }

  removeFromCache(): void {
    this.#native.removeFromCache();
  }

  selectVariant(variant: IModel): void {
    if (!(variant instanceof Model)) {
      throw new TypeError("Model.selectVariant: expected a Model instance");
    }
    const nativeVariant = nativeByModel.get(variant);
    if (nativeVariant === undefined) {
      throw new TypeError("Model.selectVariant: expected a Model instance");
    }
    this.#native.selectVariant(nativeVariant);
  }

  /**
   * @deprecated Use a `ChatSession` instead.
   * The OpenAI direct client is deprecated and will be removed at the end of 2026.
   */
  createChatClient(): ChatClient {
    return new ChatClient(this);
  }

  /**
   * @deprecated Use an `AudioSession` instead.
   * The OpenAI direct client is deprecated and will be removed at the end of 2026.
   */
  createAudioClient(): AudioClient {
    return new AudioClient(this);
  }

  /**
   * @deprecated Use an `EmbeddingsSession` instead.
   * The OpenAI direct client is deprecated and will be removed at the end of 2026.
   */
  createEmbeddingClient(): EmbeddingClient {
    return new EmbeddingClient(this);
  }
}

/** @internal — used by `Catalog` to wrap native handles into JS `Model`s. */
export function wrapNativeModel(native: NativeModel): Model {
  return new Model(internalCtorKey, native);
}

/** @internal — used by `Catalog` / `Session` to recover the native handle. */
export function unwrapNativeModel(model: Model): NativeModel {
  const native = nativeByModel.get(model);
  if (native === undefined) {
    throw new TypeError("Argument is not a valid Model handle");
  }
  return native;
}
