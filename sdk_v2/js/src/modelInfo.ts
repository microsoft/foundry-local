import type { NativeMutableModelInfo } from "./detail/native.js";
import { getAddon } from "./detail/native.js";

/** Well-known string metadata keys. Arbitrary string keys are also accepted by `setStringProperty()`. */
export const ModelInfoStringProperty = Object.freeze({
  DisplayName: "display_name",
  ModelType: "type",
  Publisher: "publisher",
  License: "license",
  LicenseDescription: "license_description",
  Task: "task",
  ModelProvider: "model_provider",
  MinFoundryLocalVersion: "min_fl_version",
  ParentUri: "parent_uri",
  ToolCallStart: "tool_call_start",
  ToolCallEnd: "tool_call_end",
  ReasoningStart: "reasoning_start",
  ReasoningEnd: "reasoning_end",
  DeviceType: "device_type",
  ExecutionProvider: "execution_provider",
  EntityType: "entity_type",
  Author: "author",
  Quantization: "quantization",
  CreationTime: "creation_time",
  InputModalities: "input_modalities",
  OutputModalities: "output_modalities",
  Capabilities: "capabilities",
} as const);

export type ModelInfoStringProperty =
  (typeof ModelInfoStringProperty)[keyof typeof ModelInfoStringProperty];

/** Well-known integer metadata keys. Boolean properties use `0` and `1`; arbitrary keys are also accepted. */
export const ModelInfoIntProperty = Object.freeze({
  SupportsToolCalling: "supports_tool_calling",
  SupportsReasoning: "supports_reasoning",
  FileSizeMb: "filesize_mb",
  MaxOutputTokens: "max_output_tokens",
  CreatedAtUnix: "created_at_unix",
  IsTestModel: "is_test_model",
  ContextLength: "context_length",
  SupportsHybridReasoning: "supports_hybrid_reasoning",
} as const);

export type ModelInfoIntProperty = (typeof ModelInfoIntProperty)[keyof typeof ModelInfoIntProperty];

const nativeByMutableModelInfo = new WeakMap<MutableModelInfo, NativeMutableModelInfo>();

/** Mutable, caller-owned metadata passed to `Catalog.registerModel()`. */
export class MutableModelInfo implements Disposable {
  readonly #native: NativeMutableModelInfo;

  constructor() {
    this.#native = new (getAddon().ModelInfo)();
    nativeByMutableModelInfo.set(this, this.#native);
  }

  /** Set a UTF-8 string property. Returns this instance for fluent construction. */
  setStringProperty(key: ModelInfoStringProperty | (string & {}), value: string): this {
    assertPropertyKey(key);
    if (typeof value !== "string") {
      throw new TypeError("ModelInfo string property value must be a string.");
    }
    this.#native.setStringProperty(key, value);
    return this;
  }

  /** Set an int64 property. JavaScript values must be safe integers. Returns this instance for fluent construction. */
  setIntProperty(key: ModelInfoIntProperty | (string & {}), value: number): this {
    assertPropertyKey(key);
    if (!Number.isSafeInteger(value)) {
      throw new TypeError("ModelInfo integer property value must be a safe integer.");
    }
    this.#native.setIntProperty(key, value);
    return this;
  }

  /** True after the native metadata handle has been released. */
  get disposed(): boolean {
    return this.#native.isDisposed();
  }

  /** Release the caller-owned native metadata handle. Idempotent. */
  dispose(): void {
    this.#native.dispose();
  }

  [Symbol.dispose](): void {
    this.dispose();
  }
}

function assertPropertyKey(key: string): void {
  if (typeof key !== "string" || key.length === 0) {
    throw new TypeError("ModelInfo property key must be a non-empty string.");
  }
}

/** @internal — unwrap mutable metadata for the native catalog binding. */
export function unwrapMutableModelInfo(info: MutableModelInfo): NativeMutableModelInfo {
  const native = nativeByMutableModelInfo.get(info);
  if (native === undefined || native.isDisposed()) {
    throw new TypeError("Catalog.registerModel: metadata must be a non-disposed ModelInfo.");
  }
  return native;
}