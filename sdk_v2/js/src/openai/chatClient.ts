// `ChatClient` and `ChatClientSettings`, layered on `ChatSession` via the OpenAI-JSON pass-through pattern.
// Mirrors C# `OpenAIChatClient` / `NativeRequestRunner` in `sdk_v2/cs/src/OpenAI/`. The request is serialised to
// OpenAI Chat Completion JSON, sent as a single `TextItem` with kind `"openai-json"`, and the response is
// recovered by JSON-parsing the first `"openai-json"` text item in the output.

import { Item, type Item as ItemT, type TextItem } from "../items.js";
import type { Model } from "../model.js";
import { Request } from "../request.js";
import type { Response } from "../response.js";
import { ChatSession } from "../session.js";
import type { ResponseFormat, ToolChoice } from "../types.js";

// biome-ignore lint/suspicious/noExplicitAny: OpenAI request/response objects are user-shaped JSON; the surface intentionally accepts `any`.
type Json = any;

export class ChatClientSettings {
  frequencyPenalty?: number;
  maxTokens?: number;
  n?: number;
  temperature?: number;
  presencePenalty?: number;
  randomSeed?: number;
  topK?: number;
  topP?: number;
  responseFormat?: ResponseFormat;
  toolChoice?: ToolChoice;

  /** @internal Serializes settings into an OpenAI-compatible request fragment. */
  _serialize(): Record<string, Json> {
    validateResponseFormat(this.responseFormat);
    validateToolChoice(this.toolChoice);

    const filterUndefined = (obj: Record<string, Json>): Record<string, Json> =>
      Object.fromEntries(Object.entries(obj).filter(([, v]) => v !== undefined));

    const result: Record<string, Json> = {
      frequency_penalty: this.frequencyPenalty,
      max_tokens: this.maxTokens,
      n: this.n,
      presence_penalty: this.presencePenalty,
      temperature: this.temperature,
      top_p: this.topP,
      response_format: this.responseFormat ? filterUndefined({ ...this.responseFormat }) : undefined,
      tool_choice: this.toolChoice ? filterUndefined({ ...this.toolChoice }) : undefined,
    };

    const metadata: Record<string, string> = {};
    if (this.topK !== undefined) {
      metadata.top_k = this.topK.toString();
    }
    if (this.randomSeed !== undefined) {
      metadata.random_seed = this.randomSeed.toString();
    }
    if (Object.keys(metadata).length > 0) {
      result.metadata = metadata;
    }

    return filterUndefined(result);
  }
}

/**
 * Client for OpenAI-style chat completions against a loaded Foundry Local model. Build via
 * {@link Model.createChatClient}. The underlying model must already be loaded — call `await model.load()` first.
 * Each `completeChat` / `completeStreamingChat` call constructs and disposes its own `ChatSession`,
 * matching the v1 SDK's stateless behaviour — no conversation state is retained on the client.
 */
export class ChatClient {
  readonly #model: Model;
  #disposed = false;

  public settings = new ChatClientSettings();

  /** @internal — construct via `model.createChatClient()`. */
  constructor(model: Model) {
    this.#model = model;
  }

  /** Modal id this client is bound to. */
  get modelId(): string {
    return this.#model.id;
  }

  /** True once `dispose()` has been called on this client. */
  get disposed(): boolean {
    return this.#disposed;
  }

  /**
   * Mark this client as disposed. Idempotent. After disposal, `completeChat` / `completeStreamingChat` throw.
   * Sessions are scoped to a single request, so there is no long-lived native resource to release here.
   */
  dispose(): void {
    this.#disposed = true;
  }

  [Symbol.dispose](): void {
    this.dispose();
  }

  #checkNotDisposed(): void {
    if (this.#disposed) {
      throw new Error("ChatClient: already disposed");
    }
  }

  /**
   * Synchronous chat completion.
   * @returns Parsed OpenAI Chat Completion response object.
   */
  async completeChat(messages: Json[]): Promise<Json>;
  async completeChat(messages: Json[], tools: Json[]): Promise<Json>;
  async completeChat(messages: Json[], tools?: Json[]): Promise<Json> {
    this.#checkNotDisposed();
    validateMessages(messages);
    validateTools(tools);

    const requestJson: Record<string, Json> = {
      model: this.modelId,
      messages,
      ...(tools ? { tools } : {}),
      ...this.settings._serialize(),
    };

    const request = new Request();
    request.addItem(Item.text(JSON.stringify(requestJson), "openai-json"));

    const session = new ChatSession(this.#model);
    let response: Response;
    try {
      response = await session.processRequest(request);
    } catch (err) {
      throw new Error(
        `Chat completion failed for model '${this.modelId}': ${err instanceof Error ? err.message : String(err)}`,
        { cause: err },
      );
    } finally {
      session.dispose();
    }

    const text = findOpenAiJsonText(response.output);
    if (text === undefined) {
      throw new Error(
        `Chat completion for model '${this.modelId}' returned no openai-json text item.`,
      );
    }
    return JSON.parse(text);
  }

  /**
   * Streaming chat completion. Yields one parsed chunk object per OpenAI
   * SSE chunk emitted by the model.
   */
  completeStreamingChat(messages: Json[]): AsyncIterable<Json>;
  completeStreamingChat(messages: Json[], tools: Json[]): AsyncIterable<Json>;
  completeStreamingChat(messages: Json[], tools?: Json[]): AsyncIterable<Json> {
    this.#checkNotDisposed();
    validateMessages(messages);
    validateTools(tools);

    const requestJson: Record<string, Json> = {
      model: this.modelId,
      messages,
      ...(tools ? { tools } : {}),
      stream: true,
      ...this.settings._serialize(),
    };

    const model = this.#model;
    const modelId = this.modelId;
    return {
      async *[Symbol.asyncIterator](): AsyncIterator<Json> {
        const request = new Request();
        request.addItem(Item.text(JSON.stringify(requestJson), "openai-json"));

        const session = new ChatSession(model);
        try {
          for await (const item of session.processStreamingRequest(request)) {
            if (item.type !== "text") {
              continue;
            }
            const textItem = item as TextItem;
            if (textItem.textType !== "openai-json" || textItem.text === "") {
              continue;
            }
            yield JSON.parse(textItem.text);
          }
        } catch (err) {
          if (err instanceof Error && err.name === "AbortError") {
            throw err;
          }
          throw new Error(
            `Streaming chat completion failed for model '${modelId}': ${err instanceof Error ? err.message : String(err)}`,
            { cause: err },
          );
        } finally {
          session.dispose();
        }
      },
    };
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

function validateMessages(messages: Json[]): void {
  if (!messages || !Array.isArray(messages) || messages.length === 0) {
    throw new Error("Messages array cannot be null, undefined, or empty.");
  }
  for (const msg of messages) {
    if (!msg || typeof msg !== "object" || Array.isArray(msg)) {
      throw new Error('Each message must be a non-null object with both "role" and "content" properties.');
    }
    if (typeof msg.role !== "string" || msg.role.trim() === "") {
      throw new Error('Each message must have a "role" property that is a non-empty string.');
    }
    if (typeof msg.content !== "string" || msg.content.trim() === "") {
      throw new Error('Each message must have a "content" property that is a non-empty string.');
    }
  }
}

function validateTools(tools?: Json[]): void {
  if (!tools) return;
  if (!Array.isArray(tools)) {
    throw new Error("Tools must be an array if provided.");
  }
  for (const tool of tools) {
    if (!tool || typeof tool !== "object" || Array.isArray(tool)) {
      throw new Error('Each tool must be a non-null object with a valid "type" and "function" definition.');
    }
    if (typeof tool.type !== "string" || tool.type.trim() === "") {
      throw new Error('Each tool must have a "type" property that is a non-empty string.');
    }
    if (!tool.function || typeof tool.function !== "object") {
      throw new Error('Each tool must have a "function" property that is a non-empty object.');
    }
    if (typeof tool.function.name !== "string" || tool.function.name.trim() === "") {
      throw new Error('Each tool\'s function must have a "name" property that is a non-empty string.');
    }
    if (tool.function.description !== undefined && typeof tool.function.description !== "string") {
      throw new Error('Each tool\'s function "description", if provided, must be a string.');
    }
  }
}

function validateResponseFormat(format?: ResponseFormat): void {
  if (!format) return;
  const validTypes = ["text", "json_object", "json_schema", "lark_grammar"];
  if (!validTypes.includes(format.type)) {
    throw new Error(`ResponseFormat type must be one of: ${validTypes.join(", ")}`);
  }
  const validGrammarTypes = ["json_schema", "lark_grammar"];
  if (validGrammarTypes.includes(format.type)) {
    if (
      format.type === "json_schema" &&
      (typeof format.jsonSchema !== "string" || format.jsonSchema.trim() === "")
    ) {
      throw new Error('ResponseFormat with type "json_schema" must have a valid jsonSchema string.');
    }
    if (
      format.type === "lark_grammar" &&
      (typeof format.larkGrammar !== "string" || format.larkGrammar.trim() === "")
    ) {
      throw new Error('ResponseFormat with type "lark_grammar" must have a valid larkGrammar string.');
    }
  } else if (format.jsonSchema || format.larkGrammar) {
    throw new Error(`ResponseFormat with type "${format.type}" should not have jsonSchema or larkGrammar properties.`);
  }
}

function validateToolChoice(choice?: ToolChoice): void {
  if (!choice) return;
  const validTypes = ["none", "auto", "required", "function"];
  if (!validTypes.includes(choice.type)) {
    throw new Error(`ToolChoice type must be one of: ${validTypes.join(", ")}`);
  }
  if (choice.type === "function" && (typeof choice.name !== "string" || choice.name.trim() === "")) {
    throw new Error('ToolChoice with type "function" must have a valid name string.');
  } else if (choice.type !== "function" && choice.name) {
    throw new Error(`ToolChoice with type "${choice.type}" should not have a name property.`);
  }
}
