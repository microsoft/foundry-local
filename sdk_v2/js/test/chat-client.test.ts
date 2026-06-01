// ChatClient (V1 OpenAI-JSON pass-through) against a real loaded chat model.
// Mirrors chat-session.test.ts shape but exercises the V1 ChatClient surface
// instead of the V2 ChatSession directly. Gated by FOUNDRY_TEST_DATA_DIR.
import { afterAll, afterEach, beforeAll, beforeEach, describe, expect, it } from "vitest";

import type { ChatClient } from "../src/openai/chatClient.js";

import {
  type RealModelManagerFixture,
  haveTestModelCache,
  setupRealModelManager,
  teardownRealModelManager,
  testModelCacheDiagnostic,
} from "./_fixtures/realModelManager.js";

if (!haveTestModelCache) {
  console.warn(testModelCacheDiagnostic);
}

// biome-ignore lint/suspicious/noExplicitAny: OpenAI response objects are user-shaped JSON.
type Json = any;

function chatChoiceText(result: Json): string {
  const choices = result?.choices;
  if (!Array.isArray(choices) || choices.length === 0) return "";
  const msg = choices[0]?.message;
  if (msg && typeof msg.content === "string") return msg.content;
  return "";
}

function chatDeltaText(chunk: Json): string {
  const choices = chunk?.choices;
  if (!Array.isArray(choices) || choices.length === 0) return "";
  const delta = choices[0]?.delta;
  if (delta && typeof delta.content === "string") return delta.content;
  return "";
}

describe.skipIf(!haveTestModelCache)("ChatClient (real model, V1 OpenAI-JSON pass-through)", () => {
  let fixture: RealModelManagerFixture | undefined;
  let client: ChatClient | undefined;

  beforeAll(async () => {
    fixture = await setupRealModelManager();
  }, 5 * 60_000);

  afterAll(() => {
    teardownRealModelManager(fixture);
  });

  beforeEach(() => {
    if (fixture === undefined) throw new Error("fixture missing");
    client = fixture.model.createChatClient();
  });

  afterEach(() => {
    client?.dispose();
    client = undefined;
  });

  it(
    "completeChat returns an OpenAI Chat Completion shape and answers the prompt",
    async () => {
      if (client === undefined) throw new Error("client missing");
      client.settings.maxTokens = 512;
      client.settings.temperature = 0;
      const result = await client.completeChat([
        { role: "system", content: "You are concise. Answer in one word." },
        { role: "user", content: "Capital of France?" },
      ]);
      expect(Array.isArray(result.choices)).toBe(true);
      expect(result.choices.length).toBeGreaterThanOrEqual(1);
      const text = chatChoiceText(result).toLowerCase();
      expect(text).toContain("paris");

      expect(result.usage).toBeDefined();
      expect(result.usage.prompt_tokens).toBeGreaterThan(0);
      expect(result.usage.completion_tokens).toBeGreaterThan(0);
      expect(result.usage.total_tokens).toBeGreaterThanOrEqual(
        result.usage.prompt_tokens + result.usage.completion_tokens,
      );
    },
    2 * 60_000,
  );

  it(
    "completeStreamingChat aggregates deltas into the final answer",
    async () => {
      if (client === undefined) throw new Error("client missing");
      client.settings.maxTokens = 512;
      client.settings.temperature = 0;

      let acc = "";
      let chunkCount = 0;
      for await (const chunk of client.completeStreamingChat([
        { role: "system", content: "You are concise. Answer in one word." },
        { role: "user", content: "Capital of France?" },
      ])) {
        chunkCount += 1;
        acc += chatDeltaText(chunk);
      }
      expect(chunkCount).toBeGreaterThan(0);
      expect(acc.toLowerCase()).toContain("paris");
    },
    2 * 60_000,
  );

  it(
    "per-call settings (temperature, maxTokens) are forwarded",
    async () => {
      if (client === undefined) throw new Error("client missing");
      client.settings.temperature = 0;
      client.settings.maxTokens = 32;
      const result = await client.completeChat([
        { role: "user", content: "Say 'ok' and nothing else." },
      ]);
      const text = chatChoiceText(result).toLowerCase();
      expect(text.length).toBeGreaterThan(0);
      // maxTokens=32 is a smoke check: completion_tokens should not blow past it by much.
      expect(result.usage.completion_tokens).toBeLessThanOrEqual(64);
    },
    2 * 60_000,
  );
});
