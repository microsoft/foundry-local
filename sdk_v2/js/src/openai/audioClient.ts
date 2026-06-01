// `AudioClient`, layered on `AudioSession` via the OpenAI-JSON pass-through pattern. Mirrors C# `OpenAIAudioClient`.

import { Item, type Item as ItemT, type TextItem } from "../items.js";
import type { Model } from "../model.js";
import { Request } from "../request.js";
import type { Response } from "../response.js";
import { AudioSession } from "../session.js";
import { LiveAudioTranscriptionSession } from "./liveAudioSession.js";

// biome-ignore lint/suspicious/noExplicitAny: OpenAI request/response objects are user-shaped JSON.
type Json = any;

export class AudioClientSettings {
  language?: string;
  temperature?: number;

  /** @internal */
  _serialize(): Record<string, Json> {
    const result: Record<string, Json> = {};
    if (this.language !== undefined) result.language = this.language;
    if (this.temperature !== undefined) result.temperature = this.temperature;
    return result;
  }
}

export class AudioClient {
  readonly #model: Model;
  #disposed = false;

  public settings = new AudioClientSettings();

  /** @internal — construct via `model.createAudioClient()`. */
  constructor(model: Model) {
    this.#model = model;
  }

  get modelId(): string {
    return this.#model.id;
  }

  /** True once `dispose()` has been called on this client. */
  get disposed(): boolean {
    return this.#disposed;
  }

  /**
   * Mark this client as disposed. Idempotent. After disposal, `transcribe` / `transcribeStreaming` throw.
   * Sessions are scoped to a single request, so there is no long-lived native resource to release here.
   * `LiveAudioTranscriptionSession` instances created via `createLiveTranscriptionSession()` have their
   * own lifetime and must be disposed separately.
   */
  dispose(): void {
    this.#disposed = true;
  }

  [Symbol.dispose](): void {
    this.dispose();
  }

  #checkNotDisposed(): void {
    if (this.#disposed) {
      throw new Error("AudioClient: already disposed");
    }
  }

  /**
   * Transcribe an audio file. Path is forwarded to native and may be any
   * format the underlying model accepts.
   */
  async transcribe(filePath: string): Promise<Json> {
    this.#checkNotDisposed();
    if (typeof filePath !== "string" || filePath.trim() === "") {
      throw new Error("filePath must be a non-empty string.");
    }
    const requestJson: Record<string, Json> = {
      model: this.modelId,
      filename: filePath,
      ...this.settings._serialize(),
    };

    const request = new Request();
    request.addItem(Item.text(JSON.stringify(requestJson), "openai-json"));

    const session = new AudioSession(this.#model);
    let response: Response;
    try {
      response = await session.processRequest(request);
    } catch (err) {
      throw new Error(
        `Audio transcription failed for model '${this.modelId}': ${err instanceof Error ? err.message : String(err)}`,
        { cause: err },
      );
    } finally {
      session.dispose();
    }

    const text = findOpenAiJsonText(response.output);
    if (text === undefined) {
      throw new Error(
        `Audio transcription for model '${this.modelId}' returned no openai-json text item.`,
      );
    }
    return JSON.parse(text);
  }

  /** Streaming transcribe. Yields parsed JSON chunks as the model emits them. */
  transcribeStreaming(filePath: string): AsyncIterable<Json> {
    this.#checkNotDisposed();
    if (typeof filePath !== "string" || filePath.trim() === "") {
      throw new Error("filePath must be a non-empty string.");
    }
    const requestJson: Record<string, Json> = {
      model: this.modelId,
      filename: filePath,
      stream: true,
      ...this.settings._serialize(),
    };

    const model = this.#model;
    const modelId = this.modelId;
    return {
      async *[Symbol.asyncIterator](): AsyncIterator<Json> {
        const request = new Request();
        request.addItem(Item.text(JSON.stringify(requestJson), "openai-json"));

        const session = new AudioSession(model);
        try {
          for await (const item of session.processStreamingRequest(request)) {
            if (item.type !== "text") continue;
            const t = item as TextItem;
            if (t.textType !== "openai-json" || t.text === "") continue;
            yield JSON.parse(t.text);
          }
        } catch (err) {
          if (err instanceof Error && err.name === "AbortError") throw err;
          throw new Error(
            `Streaming audio transcription failed for model '${modelId}': ${err instanceof Error ? err.message : String(err)}`,
            { cause: err },
          );
        } finally {
          session.dispose();
        }
      },
    };
  }

  /**
   * Create a real-time audio transcription session for streaming PCM input.
   * Call `start()`, push PCM chunks via `append(chunk)`, consume parsed
   * transcription responses via `getStream()`, then `stop()` and `dispose()`.
   */
  createLiveTranscriptionSession(): LiveAudioTranscriptionSession {
    return new LiveAudioTranscriptionSession(this.#model);
  }
}

function findOpenAiJsonText(output: ReadonlyArray<ItemT>): string | undefined {
  for (const item of output) {
    if (item.type === "text" && (item as TextItem).textType === "openai-json") {
      return (item as TextItem).text;
    }
  }
  return undefined;
}
