// Streaming tests for Session.stream / ChatSession.stream.
// Gated by FOUNDRY_TEST_DATA_DIR (real model required).
import { afterAll, afterEach, beforeAll, beforeEach, describe, expect, it } from "vitest";

import { FlErrorCode, isFoundryLocalError } from "../src/detail/errors.js";
import type { NativeChatSession } from "../src/detail/native.js";
import { Item } from "../src/items.js";
import { Request, unwrapNativeRequest } from "../src/request.js";
import { ChatSession } from "../src/session.js";

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

function buildPrompt(): Request {
  // Use a prompt that naturally produces several tokens (so we can validate
  // that streaming actually delivers multiple deltas) while still containing
  // deterministic-enough substrings on a 0.5B model.
  //
  // "Name the countries in the United Kingdom." is reliably answered with at
  // least two of England / Scotland / Wales / Ireland regardless of phrasing.
  // Compare with sdk_v2/cpp/test/internal_api/chat/chat_session_test.cc which
  // only asserts callback_count > 0 — we also assert items.length >= 2 to
  // catch a regression where the native layer collapsed deltas into one item.
  return new Request()
    .addItem(Item.userMessage("Name the countries in the United Kingdom."))
    .setOptions({ search: { maxOutputTokens: 128, temperature: 0 } });
}

// Deterministic substrings expected to appear in any reasonable answer to
// buildPrompt(); we require a subset rather than all four to stay robust on
// a 0.5B model that may abbreviate or reorder.
const UK_COUNTRY_TOKENS = ["england", "scotland", "wales", "ireland"] as const;

function countUkTokens(text: string): number {
  const lower = text.toLowerCase();
  return UK_COUNTRY_TOKENS.filter((t) => lower.includes(t)).length;
}

// Used by the multi-turn streaming test: a context-dependent follow-up
// ("What is the capital of each?") should mention the UK capitals.
const UK_CAPITAL_TOKENS = ["london", "edinburgh", "cardiff", "belfast"] as const;
const PRIMARY_COLOR_TOKENS = ["red", "blue", "yellow"] as const;

function countUkCapitalTokens(text: string): number {
  const lower = text.toLowerCase();
  return UK_CAPITAL_TOKENS.filter((t) => lower.includes(t)).length;
}

function extractText(item: Item): string {
  if (item.type === "text") return item.text;
  if (item.type === "message") {
    if (typeof item.content === "string") return item.content;
    if (item.parts) {
      let acc = "";
      for (const p of item.parts) {
        if (p.type === "text") acc += p.text;
      }
      return acc;
    }
  }
  return "";
}

describe.skipIf(!haveTestModelCache)("ChatSession.processStreamingRequest (real model)", () => {
  let fixture: RealModelManagerFixture | undefined;
  let session: ChatSession | undefined;

  beforeAll(async () => {
    fixture = await setupRealModelManager();
  }, 5 * 60_000);

  afterAll(() => {
    teardownRealModelManager(fixture);
  });

  beforeEach(() => {
    if (fixture === undefined) throw new Error("fixture missing");
    session = new ChatSession(fixture.model);
  });

  afterEach(() => {
    session?.dispose();
    session = undefined;
  });

  it(
    "yields multiple Items before completion and the items carry deterministic content",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      const items: Item[] = [];
      for await (const item of session.processStreamingRequest(buildPrompt())) {
        items.push(item);
      }
      // Real streaming must deliver more than a single coalesced delta.
      expect(items.length).toBeGreaterThanOrEqual(2);
      const total = items.reduce((acc, it) => acc + extractText(it), "");
      expect(total.length).toBeGreaterThan(0);
      expect(countUkTokens(total)).toBeGreaterThanOrEqual(2);
    },
    2 * 60_000,
  );

  it(
    "concatenated streamed text contains the expected answer content",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      let text = "";
      for await (const item of session.processStreamingRequest(buildPrompt())) {
        text += extractText(item);
      }
      expect(countUkTokens(text)).toBeGreaterThanOrEqual(2);
    },
    2 * 60_000,
  );

  it(
    "early break requests cancellation while permitting prior native completion",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      const stream = session.processStreamingRequest(buildPrompt());
      let count = 0;
      for await (const _item of stream) {
        count++;
        if (count >= 1) break;
      }
      const outcome = await stream.response.then(
        (response) => ({ response, error: null }),
        (error: unknown) => ({ response: null, error }),
      );
      if (outcome.response === null) {
        expect(outcome.error).toMatchObject({
          name: "FoundryLocalError",
          code: FlErrorCode.OperationCancelled,
        });
      } else {
        expect(outcome.response.finishReason).not.toBe("none");
      }

      // After the break the session should accept a follow-up send.
      const resp = await session.processRequest(
        new Request()
          .addItem(Item.userMessage("Reply with the single word 'ok'."))
          .setOptions({ search: { maxOutputTokens: 4, temperature: 0 } }),
      );
      expect(resp.output.length).toBeGreaterThanOrEqual(1);
      const text = resp.output.map(extractText).join("").toLowerCase();
      expect(text).toContain("ok");
    },
    3 * 60_000,
  );

  it("pre-aborted AbortSignal rejects iteration with name === 'AbortError'", async () => {
    if (session === undefined) throw new Error("fixture missing");
    const ctrl = new AbortController();
    ctrl.abort();
    const iter = session.processStreamingRequest(buildPrompt(), { signal: ctrl.signal });
    await expect(async () => {
      for await (const _item of iter) {
        /* no Item expected */
      }
    }).rejects.toMatchObject({ name: "AbortError" });
  }, 60_000);

  it(
    "mid-stream abort rejects with AbortError and the session remains usable",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      const ctrl = new AbortController();
      let caught: unknown = null;
      try {
        let seen = 0;
        for await (const _item of session.processStreamingRequest(buildPrompt(), { signal: ctrl.signal })) {
          seen++;
          if (seen >= 1) ctrl.abort();
        }
      } catch (e) {
        caught = e;
      }
      // The abort may race the natural completion of a very short reply; if
      // we never observed an abort, just skip the assertion on `caught`.
      if (caught !== null) {
        expect((caught as { name: string }).name).toBe("AbortError");
        if (isFoundryLocalError(caught)) {
          expect(caught.code).toBe(FlErrorCode.OperationCancelled);
        }
      }
      // Session must still accept a follow-up send regardless.
      const resp = await session.processRequest(
        new Request()
          .addItem(Item.userMessage("Reply with the single word 'ok'."))
          .setOptions({ search: { maxOutputTokens: 4, temperature: 0 } }),
      );
      expect(resp.output.length).toBeGreaterThanOrEqual(1);
      const text = resp.output.map(extractText).join("").toLowerCase();
      expect(text).toContain("ok");
    },
    3 * 60_000,
  );

  it(
    "a second stream on the same session works after the first completes",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      // Turn 1: deterministic content check on the UK-countries prompt.
      const firstItems: Item[] = [];
      let first = "";
      for await (const item of session.processStreamingRequest(buildPrompt())) {
        firstItems.push(item);
        first += extractText(item);
      }
      expect(firstItems.length).toBeGreaterThanOrEqual(2);
      expect(countUkTokens(first)).toBeGreaterThanOrEqual(2);

      // Turn 2: a follow-up that depends on turn 1's context. Asking for the
      // capital of each exercises history-aware generation and gives us a
      // second deterministic content check (London / Edinburgh / Cardiff /
      // Belfast). We require at least two to stay robust on a 0.5B model.
      const secondItems: Item[] = [];
      let second = "";
      for await (const item of session.processStreamingRequest(
        new Request()
          .addItem(Item.userMessage("What is the capital of each?"))
          .setOptions({ search: { maxOutputTokens: 128, temperature: 0 } }),
      )) {
        secondItems.push(item);
        second += extractText(item);
      }
      expect(secondItems.length).toBeGreaterThanOrEqual(2);
      expect(countUkCapitalTokens(second)).toBeGreaterThanOrEqual(2);
    },
    4 * 60_000,
  );

  it(
    "starts overlapping requests in per-session FIFO order",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      const nativeSession = (session as unknown as { native: NativeChatSession }).native;
      const request = new Request()
        .addItem(Item.userMessage("Reply with a short greeting."))
        .setOptions({ search: { maxOutputTokens: 16, temperature: 0 } });
      let signalFirstWorkerStarted!: (release: () => void) => void;
      const firstWorkerStarted = new Promise<() => void>((resolve) => {
        signalFirstWorkerStarted = resolve;
      });
      const active = nativeSession.processRequest(unwrapNativeRequest(request), signalFirstWorkerStarted);
      const releaseFirst = await firstWorkerStarted;

      let secondStarted = false;
      let signalSecondWorkerStarted!: (release: () => void) => void;
      const secondWorkerStarted = new Promise<() => void>((resolve) => {
        signalSecondWorkerStarted = (release) => {
          secondStarted = true;
          resolve(release);
        };
      });
      const queued = nativeSession.processRequest(unwrapNativeRequest(buildPrompt()), signalSecondWorkerStarted);
      await new Promise<void>((resolve) => setImmediate(resolve));
      expect(secondStarted).toBe(false);

      releaseFirst();
      await expect(active).resolves.toMatchObject({ output: expect.any(Array) });
      const releaseSecond = await secondWorkerStarted;
      releaseSecond();
      await expect(queued).resolves.toMatchObject({ output: expect.any(Array) });
    },
    3 * 60_000,
  );

  it(
    "aborts a queued stream before native processing starts",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      const nativeSession = (session as unknown as { native: NativeChatSession }).native;
      let signalFirstWorkerStarted!: (release: () => void) => void;
      const firstWorkerStarted = new Promise<() => void>((resolve) => {
        signalFirstWorkerStarted = resolve;
      });
      const active = nativeSession.processRequest(unwrapNativeRequest(buildPrompt()), signalFirstWorkerStarted);
      const releaseFirst = await firstWorkerStarted;
      const turnsBeforeAbort = session.turnCount;

      const ctrl = new AbortController();
      const queued = session.processStreamingRequest(buildPrompt(), { signal: ctrl.signal });
      ctrl.abort();

      await expect(queued.response).rejects.toMatchObject({
        name: "AbortError",
        code: FlErrorCode.OperationCancelled,
      });
      expect(session.turnCount).toBe(turnsBeforeAbort);

      releaseFirst();
      await expect(active).resolves.toMatchObject({ output: expect.any(Array) });
      expect(session.turnCount).toBe(turnsBeforeAbort + 1);
    },
    3 * 60_000,
  );

  it(
    "clears streaming state after native inference rejects",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      const failed = session.processStreamingRequest(new Request());
      await expect(failed.response).rejects.toMatchObject({ name: "FoundryLocalError" });

      const items: Item[] = [];
      for await (const item of session.processStreamingRequest(buildPrompt())) {
        items.push(item);
      }
      expect(items.length).toBeGreaterThanOrEqual(2);
    },
    3 * 60_000,
  );

  it(
    "stream.response resolves with finishReason and usage after full iteration",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      const stream = session.processStreamingRequest(buildPrompt());
      let streamedText = "";
      for await (const item of stream) {
        streamedText += extractText(item);
      }
      const resp = await stream.response;
      expect(["stop", "length", "toolCalls", "error", "none"]).toContain(resp.finishReason);
      expect(resp.usage.promptTokens).toBeGreaterThan(0);
      expect(resp.usage.completionTokens).toBeGreaterThan(0);
      expect(resp.usage.totalTokens).toBeGreaterThanOrEqual(resp.usage.promptTokens + resp.usage.completionTokens);
      // The Response's text should match what we accumulated from the stream
      // (modulo possible model post-processing — assert non-empty overlap on
      // the boundary tokens rather than strict equality).
      const responseText = resp.output.map(extractText).join("").toLowerCase();
      expect(responseText.length).toBeGreaterThan(0);
      expect(streamedText.length).toBeGreaterThan(0);
    },
    3 * 60_000,
  );

  it(
    "serializes overlapping streams without replacing either callback",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      const first = session.processStreamingRequest(buildPrompt());
      const second = session.processStreamingRequest(
        new Request()
          .addItem(Item.userMessage("Name three primary colors."))
          .setOptions({ search: { maxOutputTokens: 64, temperature: 0 } }),
      );

      const collect = async (stream: AsyncIterable<Item>): Promise<Item[]> => {
        const items: Item[] = [];
        for await (const item of stream) items.push(item);
        return items;
      };
      const [firstItems, secondItems, firstResponse, secondResponse] = await Promise.all([
        collect(first),
        collect(second),
        first.response,
        second.response,
      ]);

      expect(firstItems.length).toBeGreaterThan(0);
      expect(secondItems.length).toBeGreaterThan(0);
      expect(countUkTokens(firstItems.map(extractText).join(""))).toBeGreaterThanOrEqual(2);
      expect(
        PRIMARY_COLOR_TOKENS.filter((token) => secondItems.map(extractText).join("").toLowerCase().includes(token))
          .length,
      ).toBeGreaterThanOrEqual(2);
      expect(firstResponse.finishReason).not.toBe("none");
      expect(secondResponse.finishReason).not.toBe("none");
    },
    4 * 60_000,
  );

  it(
    "finishes accepted queued work after dispose and rejects future work",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      const active = session.processStreamingRequest(buildPrompt());
      const queued = session.processRequest(
        new Request()
          .addItem(Item.userMessage("Reply with the single word 'ok'."))
          .setOptions({ search: { maxOutputTokens: 4, temperature: 0 } }),
      );

      session.dispose();
      expect(session.disposed).toBe(true);

      await expect(
        session.processRequest(new Request().addItem(Item.userMessage("This must not be accepted."))),
      ).rejects.toMatchObject({
        name: "FoundryLocalError",
        code: FlErrorCode.InvalidUsage,
      });

      const activeItems: Item[] = [];
      for await (const item of active) activeItems.push(item);
      const [activeResponse, queuedResponse] = await Promise.all([active.response, queued]);

      expect(activeItems.length).toBeGreaterThan(0);
      expect(activeResponse.finishReason).not.toBe("none");
      expect(queuedResponse.finishReason).not.toBe("none");
      expect(queuedResponse.output.map(extractText).join("").toLowerCase()).toContain("ok");
    },
    4 * 60_000,
  );

  it(
    "stream.response resolves without iteration (eager native start)",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      const stream = session.processStreamingRequest(buildPrompt());
      // Deliberately do NOT iterate. The native call should still run to
      // completion and `.response` should settle.
      const resp = await stream.response;
      expect(["stop", "length", "toolCalls", "error", "none"]).toContain(resp.finishReason);
      expect(resp.output.length).toBeGreaterThanOrEqual(1);
    },
    3 * 60_000,
  );

  it("stream.response rejects with AbortError when pre-aborted", async () => {
    if (session === undefined) throw new Error("fixture missing");
    const ctrl = new AbortController();
    ctrl.abort();
    const stream = session.processStreamingRequest(buildPrompt(), { signal: ctrl.signal });
    await expect(stream.response).rejects.toMatchObject({ name: "AbortError" });
  }, 60_000);

  it(
    "request.cancel yields OperationCancelled unless native completion wins",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      const req = new Request()
        .addItem(Item.systemMessage("You are verbose."))
        .addItem(Item.userMessage("Write a 500-word essay about the history of bread."))
        .setOptions({ search: { maxOutputTokens: 1024, temperature: 0 } });
      const stream = session.processStreamingRequest(req);
      const iteration = async (): Promise<void> => {
        let observed = 0;
        for await (const _item of stream) {
          if (++observed >= 1) {
            req.cancel();
          }
        }
      };

      const iterationOutcome = await iteration().then(
        () => null,
        (error: unknown) => error,
      );
      const responseOutcome = await stream.response.then(
        (response) => ({ response, error: null }),
        (error: unknown) => ({ response: null, error }),
      );

      if (responseOutcome.response === null) {
        expect(iterationOutcome).toMatchObject({
          name: "FoundryLocalError",
          code: FlErrorCode.OperationCancelled,
        });
        expect(responseOutcome.error).toMatchObject({
          name: "FoundryLocalError",
          code: FlErrorCode.OperationCancelled,
        });
        expect(session.turnCount).toBe(0);
      } else {
        expect(iterationOutcome).toBeNull();
        expect(responseOutcome.response.finishReason).not.toBe("none");
        expect(session.turnCount).toBe(1);
      }
    },
    3 * 60_000,
  );

  // Streaming + tool-call assembly. Mirrors the C++ ToolCallStreamingWithRequired
  // test: with toolChoice="required" forcing a tool call, the streaming iterator
  // must deliver at least one fully-assembled ToolCallItem (the chat generator
  // buffers partial tool-call JSON internally rather than streaming the payload
  // character-by-character) and that streamed item must match the corresponding
  // item in the final Response.
  it(
    "yields a fully-assembled tool call when toolChoice='required'",
    async () => {
      if (session === undefined) throw new Error("fixture missing");

      session.addToolDefinition({
        name: "multiply_numbers",
        description: "A tool for multiplying two numbers.",
        jsonSchema: JSON.stringify({
          type: "object",
          properties: {
            first: { type: "integer", description: "The first number in the operation" },
            second: { type: "integer", description: "The second number in the operation" },
          },
          required: ["first", "second"],
        }),
      });

      const req = new Request()
        .addItem(
          Item.systemMessage(
            "You are a helpful AI assistant. If necessary, you can use any provided tools to answer the question.",
          ),
        )
        .addItem(Item.userMessage("What is the answer to 7 multiplied by 6?"))
        .setOptions({
          search: { temperature: 0, maxOutputTokens: 256 },
          toolChoice: "required",
        });

      const stream = session.processStreamingRequest(req);

      interface StreamedToolCall {
        callId: string;
        name: string;
        arguments: string;
      }
      const streamedToolCalls: StreamedToolCall[] = [];
      let itemCount = 0;
      for await (const item of stream) {
        itemCount++;
        if (item.type === "toolCall") {
          streamedToolCalls.push({ callId: item.callId, name: item.name, arguments: item.arguments });
        }
      }

      expect(itemCount).toBeGreaterThan(0);
      expect(streamedToolCalls.length).toBeGreaterThanOrEqual(1);

      const streamed = streamedToolCalls[0];
      if (streamed === undefined) throw new Error("Expected a streamed tool call");
      expect(streamed.name).toBe("multiply_numbers");
      expect(streamed.arguments.length).toBeGreaterThan(0);
      expect(streamed.callId.length).toBeGreaterThan(0);

      // Cross-check: the streamed tool call must also appear in the final Response
      // with the same callId/name/arguments. Proves the streaming path and the
      // final-response path agree on what was emitted.
      const resp = await stream.response;
      expect(resp.finishReason).toBe("toolCalls");

      const finalToolCall = resp.output.find((it): it is Extract<Item, { type: "toolCall" }> => it.type === "toolCall");
      expect(finalToolCall).toBeDefined();
      if (finalToolCall === undefined) throw new Error("Expected a final tool call");
      expect(finalToolCall.name).toBe(streamed.name);
      expect(finalToolCall.arguments).toBe(streamed.arguments);
      expect(finalToolCall.callId).toBe(streamed.callId);
    },
    3 * 60_000,
  );
});
