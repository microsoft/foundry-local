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
  get isCached(): boolean;
  isLoaded(): Promise<boolean>;

  get contextLength(): number | null;
  get inputModalities(): string | null;
  get outputModalities(): string | null;
  get capabilities(): string | null;
  get supportsToolCalling(): boolean | null;

  download(progressCallback?: (progress: number) => void): Promise<void>;
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
