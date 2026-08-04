// Public `Response` shape returned by `Session.processRequest`. The object is
// plain JS; zero-copy binary views transparently retain their native storage.
import type { Item } from "./items.js";

/** Reason inference stopped. */
export type FinishReason = "none" | "stop" | "length" | "toolCalls" | "error";

export interface TokenUsage {
  readonly promptTokens: number;
  readonly completionTokens: number;
  readonly totalTokens: number;
}

export interface Response {
  readonly output: ReadonlyArray<Item>;
  readonly finishReason: FinishReason;
  readonly usage: TokenUsage;
}
