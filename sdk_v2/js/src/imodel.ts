// Model interface. Use the OpenAI `ChatClient` / `AudioClient` / `EmbeddingClient` for inference; they talk to
// the native session via the OPENAI_JSON text-item pass-through pattern.

import type { AudioClient } from "./openai/audioClient.js";
import type { ChatClient } from "./openai/chatClient.js";
import type { EmbeddingClient } from "./openai/embeddingClient.js";
import type { ModelInfo } from "./types.js";

export interface IModel {
  get id(): string;
  get alias(): string;
  get info(): ModelInfo;
  getStringProperty(key: string): string | undefined;
  getIntProperty(key: string, defaultValue?: number): number;
  get isCached(): boolean;
  isLoaded(): Promise<boolean>;

  get contextLength(): number | null;
  get inputModalities(): string | null;
  get outputModalities(): string | null;
  get capabilities(): string | null;
  get supportsToolCalling(): boolean | null;

  /**
   * Downloads the model. Cancellation is cooperative and is observed at native download progress checkpoints.
   * Registry resolution, blob listing, or blob property lookup already in progress must finish before cancellation is observed.
   */
  download(signal?: AbortSignal): Promise<void>;
  download(progressCallback?: (progress: number) => void, signal?: AbortSignal): Promise<void>;
  get path(): string;
  load(): Promise<void>;
  removeFromCache(): void;
  unload(): Promise<void>;

  createChatClient(): ChatClient;
  createAudioClient(): AudioClient;
  createEmbeddingClient(): EmbeddingClient;

  /** Variants of the model, optimized for different device + EP combos. */
  get variants(): IModel[];

  /**
   * Select a model variant. Must be one of the variants in `variants`.
   * @throws Error if `variant` is not valid for this model.
   */
  selectVariant(variant: IModel): void;
}
