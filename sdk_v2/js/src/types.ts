// Types shared across the surface. New code should prefer the typed `Item` / `Session` surfaces.

export enum DeviceType {
  Invalid = "Invalid",
  CPU = "CPU",
  GPU = "GPU",
  NPU = "NPU",
}

/**
 * @deprecated PromptTemplate is an internal model implementation detail and will be removed in a future release.
 * Templates are applied automatically by ChatSession.
 */
export interface PromptTemplate {
  system?: string | null;
  user?: string | null;
  assistant: string;
  prompt: string;
}

export interface Runtime {
  deviceType: DeviceType;
  executionProvider: string;
}

export interface Parameter {
  name: string;
  value?: string | null;
}

export interface ModelSettings {
  parameters?: Parameter[] | null;
}

export interface ModelInfo {
  readonly id: string;
  readonly name: string;
  readonly version: number;
  readonly alias: string;
  readonly uri: string;
  readonly deviceType: DeviceType;
  readonly executionProvider?: string;
  readonly displayName?: string;
  readonly modelType?: string;
  readonly providerType: string;
  /**
   * @deprecated PromptTemplate is an internal model implementation detail and will be removed in a future release.
   * Templates are applied automatically by ChatSession.
   */
  readonly promptTemplate?: PromptTemplate | null;
  readonly publisher?: string;
  readonly modelSettings?: ModelSettings | null;
  readonly license?: string;
  readonly licenseDescription?: string;
  readonly cached: boolean;
  readonly task?: string;
  readonly runtime?: Runtime | null;
  readonly modelProvider?: string;
  readonly minFLVersion?: string;
  readonly parentUri?: string;
  readonly supportsToolCalling?: boolean;
  readonly fileSizeMb?: number;
  readonly maxOutputTokens?: number;
  readonly createdAtUnix: number;
  readonly isTestModel: boolean;
  readonly contextLength?: number;
  readonly inputModalities?: string;
  readonly outputModalities?: string;
  readonly capabilities?: string;
}

/** Information about a discoverable execution provider. */
export interface EpInfo {
  readonly name: string;
  readonly isRegistered: boolean;
}

/** Result of a `downloadAndRegisterEps()` call. */
export interface EpDownloadResult {
  readonly success: boolean;
  readonly status: string;
  readonly registeredEps: readonly string[];
  readonly failedEps: readonly string[];
}

/** OpenAI-compatible response format hint. */
export interface ResponseFormat {
  type: "text" | "json_object" | "json_schema" | "lark_grammar";
  jsonSchema?: string;
  larkGrammar?: string;
}

/** OpenAI-compatible tool choice descriptor. */
export interface ToolChoice {
  type: "none" | "auto" | "required" | "function";
  name?: string;
}
