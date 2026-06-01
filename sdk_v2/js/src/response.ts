// Public `Response` shape returned by `Session.send`. A plain JS object —
// no native backing, safe to retain past the session's lifetime.
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
