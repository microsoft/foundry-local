// `EmbeddingClient`, layered on `EmbeddingsSession` via the OpenAI-JSON pass-through pattern. Mirrors C#
// `OpenAIEmbeddingClient`.

import { Item, type Item as ItemT, type TextItem } from "../items.js";
import type { Model } from "../model.js";
import { Request } from "../request.js";
import type { Response } from "../response.js";
import { EmbeddingsSession } from "../session.js";

// biome-ignore lint/suspicious/noExplicitAny: OpenAI request/response objects are user-shaped JSON.
type Json = any;

export class EmbeddingClient {
  readonly #model: Model;
  #disposed = false;

  /** @internal — construct via `model.createEmbeddingClient()`. */
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
   * Mark this client as disposed. Idempotent. After disposal, `generateEmbedding` / `generateEmbeddings`
   * throw. Sessions are scoped to a single request, so there is no long-lived native resource to release here.
   */
  dispose(): void {
    this.#disposed = true;
  }

  [Symbol.dispose](): void {
    this.dispose();
  }

  /** Generate an embedding for a single input. */
  async generateEmbedding(input: string): Promise<Json> {
    if (typeof input !== "string" || input.length === 0) {
      throw new Error("Embedding input must be a non-empty string.");
    }
    return this.#dispatch(input);
  }

  /** Generate embeddings for an array of inputs (batch). */
  async generateEmbeddings(inputs: string[]): Promise<Json> {
    if (!inputs || !Array.isArray(inputs) || inputs.length === 0) {
      throw new Error("Embedding inputs must be a non-empty array of strings.");
    }
    for (const s of inputs) {
      if (typeof s !== "string" || s.length === 0) {
        throw new Error("Every embedding input must be a non-empty string.");
      }
    }
    return this.#dispatch(inputs);
  }

  async #dispatch(input: string | string[]): Promise<Json> {
    if (this.#disposed) {
      throw new Error("EmbeddingClient: already disposed");
    }
    const request = new Request();
    request.addItem(
      Item.text(JSON.stringify({ model: this.modelId, input }), "openai-json"),
    );

    const session = new EmbeddingsSession(this.#model);
    let response: Response;
    try {
      response = await session.processRequest(request);
    } catch (err) {
      throw new Error(
        `Embedding generation failed for model '${this.modelId}': ${err instanceof Error ? err.message : String(err)}`,
        { cause: err },
      );
    } finally {
      session.dispose();
    }

    const text = findOpenAiJsonText(response.output);
    if (text === undefined) {
      throw new Error(
        `Embedding generation for model '${this.modelId}' returned no openai-json text item.`,
      );
    }
    return JSON.parse(text);
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
