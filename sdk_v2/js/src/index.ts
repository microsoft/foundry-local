// Public entry for foundry-local-sdk.

export { FoundryLocalManager } from "./foundryLocalManager.js";
export type { FoundryLocalConfig } from "./configuration.js";
export { Catalog } from "./catalog.js";
export { Model } from "./model.js";
export type { IModel } from "./imodel.js";
export type {
  DeviceType,
  EpDownloadResult,
  EpInfo,
  ModelInfo,
  ModelSettings,
  Parameter,
  PromptTemplate,
  ResponseFormat,
  Runtime,
  ToolChoice,
} from "./types.js";

export { ChatClient, ChatClientSettings } from "./openai/chatClient.js";
export { AudioClient, AudioClientSettings } from "./openai/audioClient.js";
export { EmbeddingClient } from "./openai/embeddingClient.js";
export {
  LiveAudioTranscriptionOptions,
  LiveAudioTranscriptionSession,
} from "./openai/liveAudioSession.js";
export type {
  LiveAudioTranscriptionResponse,
  TranscriptionContentPart,
} from "./openai/liveAudioTypes.js";

export { FlErrorCode, isFoundryLocalError, type FoundryLocalError } from "./detail/errors.js";
export { configureNativeLoader } from "./detail/native.js";

export {
  Session,
  ChatSession,
  EmbeddingsSession,
  AudioSession,
  type ToolDefinition,
  type StreamOptions,
  type StreamingResponse,
} from "./session.js";
export { Request, type RequestOptions, type RequestToolChoice, type SearchOptions } from "./request.js";
export type { Response, FinishReason, TokenUsage } from "./response.js";
export { ItemQueue } from "./item-queue.js";

export {
  Item,
  type AudioItem,
  type BytesItem,
  type ImageItem,
  type MessageItem,
  type MessageRole,
  type TensorDataType,
  type TensorItem,
  type TextItem,
  type TextItemKind,
  type ToolCallItem,
  type ToolResultItem,
} from "./items.js";
