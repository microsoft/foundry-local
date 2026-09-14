// Non-streaming ChatSession tests against a real loaded chat model.
// Gated by FOUNDRY_TEST_DATA_DIR. Streaming + AbortSignal coverage lives in
// streaming.test.ts.
import { rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { afterAll, afterEach, beforeAll, beforeEach, describe, expect, expectTypeOf, it } from "vitest";

import { ItemQueue } from "../src/item-queue.js";
import { Item } from "../src/items.js";
import { Request } from "../src/request.js";
import type { RequestPreflight } from "../src/session.js";
import { ChatSession } from "../src/session.js";

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

function outputText(output: ReadonlyArray<Item>): string {
  return output.map(extractText).join("");
}

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

describe.skipIf(!haveTestModelCache)("ChatSession (real model, non-streaming)", () => {
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
    "preflightRequest() returns a stable budget without changing conversation history",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      const req = new Request()
        .addItem(Item.systemMessage("You are concise."))
        .addItem(Item.userMessage("Capital of France?"))
        .setOptions({ search: { maxOutputTokens: 32, temperature: 0 } });
      const turnsBefore = session.turnCount;

      const result = await session.preflightRequest(req);
      const repeated = await session.preflightRequest(req);

      expect(result).toEqual(repeated);
      expect(session.turnCount).toBe(turnsBefore);
      expect(Number.isInteger(result.promptTokens)).toBe(true);
      expect(result.promptTokens).toBeGreaterThan(0);
      expect(Number.isInteger(result.outputReserveTokens)).toBe(true);
      expect(result.outputReserveTokens).toBe(32);
      expect(result.requiredTokens).toBe(result.promptTokens + result.outputReserveTokens);
      expect(result.contextLimitTokens).toBeGreaterThan(0);
      expect(result.deficitTokens).toBeGreaterThanOrEqual(0);
      expect(result.fits).toBe(result.deficitTokens === 0);
      expect(Object.keys(result).sort()).toEqual([
        "contextLimitTokens",
        "deficitTokens",
        "fits",
        "outputReserveTokens",
        "promptTokens",
        "requiredTokens",
      ]);
    },
    2 * 60_000,
  );

  it(
    "preflightRequest() isolates in-flight work from later request and chat-session mutations",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      let settled = false;
      const request = new Request()
        .addItem(Item.userMessage("Count the tokens in this request."))
        .setOptions({ search: { maxOutputTokens: 8, temperature: 0 } });
      const pending = session.preflightRequest(request);
      pending.finally(() => {
        settled = true;
      });

      request
        .addItem(
          Item.userMessage(
            "This deliberately longer follow-up is added only after the first invocation and must increase the next count.",
          ),
        )
        .setOptions({ search: { maxOutputTokens: 17, temperature: 0 } });
      session.setOptions({ search: { temperature: 0.25 } }).addToolDefinition({
        name: "snapshot_state_probe",
        description: "A deterministic tool definition added only after the first preflight invocation.",
        jsonSchema: JSON.stringify({
          type: "object",
          properties: {
            value: { type: "string", description: "A value used to verify snapshot isolation." },
          },
          required: ["value"],
        }),
      });

      expect(pending).toBeInstanceOf(Promise);
      await Promise.resolve();
      expect(settled).toBe(false);
      const original = await pending;
      const mutated = await session.preflightRequest(request);

      expect(original.outputReserveTokens).toBe(8);
      expect(mutated.outputReserveTokens).toBe(17);
      expect(mutated.promptTokens).toBeGreaterThan(original.promptTokens);
    },
    2 * 60_000,
  );

  it(
    "preflightRequest() rejects asynchronously when the request cannot be preflighted",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      using queue = new ItemQueue();
      queue.push(Item.text("queued input cannot be preflighted"));
      const request = new Request().addItem(queue);
      const activeSession = session;

      let pending: Promise<RequestPreflight> | undefined;
      expect(() => {
        pending = activeSession.preflightRequest(request);
      }).not.toThrow();

      expect(pending).toBeInstanceOf(Promise);
      if (pending === undefined) throw new Error("preflightRequest did not return");
      await expect(pending).rejects.toMatchObject({
        name: "FoundryLocalError",
      });
      expect(queue.size).toBe(1);
    },
    2 * 60_000,
  );

  it(
    "preflightRequest() reads URI content asynchronously and reports failures",
    async ({ skip }) => {
      if (fixture === undefined) throw new Error("fixture missing");
      const visionModel = (await fixture.catalog.getModels()).find(
        (model) => model.info.task === "vision-language-chat" && model.isCached,
      );
      if (visionModel === undefined) {
        skip("URI preflight requires a cached vision-language-chat model");
        return;
      }
      await visionModel.load();
      using visionSession = new ChatSession(visionModel);
      const missingImage = join(tmpdir(), `foundry-local-js-preflight-missing-${process.pid}-${Date.now()}.png`);
      rmSync(missingImage, { force: true });
      const request = new Request().addItem(
        Item.userMessage([Item.text("Describe this image."), Item.imageFromUri(missingImage, "png")]),
      );

      let pending: Promise<RequestPreflight> | undefined;
      expect(() => {
        pending = visionSession.preflightRequest(request);
      }).not.toThrow();

      expect(pending).toBeInstanceOf(Promise);
      if (pending === undefined) throw new Error("preflightRequest did not return");
      try {
        await pending;
        throw new Error("preflightRequest unexpectedly succeeded");
      } catch (error) {
        expect(error).toMatchObject({ name: "FoundryLocalError" });
        expect(error).toBeInstanceOf(Error);
        if (!(error instanceof Error)) throw error;
        expect(error.message).toMatch(/open image file/i);
      }
    },
    2 * 60_000,
  );

  it(
    "preflightRequest() completes after its session is disposed",
    async () => {
      if (fixture === undefined) throw new Error("fixture missing");
      const oneShot = new ChatSession(fixture.model);
      const request = new Request()
        .addItem(Item.userMessage("Count the tokens in this request."))
        .setOptions({ search: { maxOutputTokens: 8 } });

      const pending = oneShot.preflightRequest(request);
      oneShot.dispose();

      expect(oneShot.disposed).toBe(true);
      await expect(pending).resolves.toMatchObject({
        outputReserveTokens: 8,
      });
    },
    2 * 60_000,
  );

  it(
    "processRequest() resolves with a Response that contains at least one output item",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      const req = new Request()
        .addItem(Item.systemMessage("You are concise. Answer in one word."))
        .addItem(Item.userMessage("Capital of France?"))
        .setOptions({ search: { maxOutputTokens: 512, temperature: 0 } });
      const resp = await session.processRequest(req);
      expect(resp.output.length).toBeGreaterThanOrEqual(1);
      expect(["stop", "length", "toolCalls", "error", "none"]).toContain(resp.finishReason);
      expect(outputText(resp.output).toLowerCase()).toContain("paris");
      expect(resp.usage.promptTokens).toBeGreaterThan(0);
      expect(resp.usage.completionTokens).toBeGreaterThan(0);
      expect(resp.usage.totalTokens).toBeGreaterThanOrEqual(resp.usage.promptTokens + resp.usage.completionTokens);
    },
    2 * 60_000,
  );

  it(
    "turnCount increases after a successful processRequest",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      const before = session.turnCount;
      expect(before).toBe(0);
      const req = new Request()
        .addItem(Item.userMessage("Say 'ok' and nothing else."))
        .setOptions({ search: { maxOutputTokens: 256, temperature: 0 } });
      const resp = await session.processRequest(req);
      expect(session.turnCount).toBeGreaterThan(before);
      expect(outputText(resp.output).toLowerCase()).toContain("ok");
    },
    2 * 60_000,
  );

  it(
    "undoTurns rewinds the conversation",
    async () => {
      if (session === undefined) throw new Error("fixture missing");
      await session.processRequest(
        new Request().addItem(Item.userMessage("hi")).setOptions({ search: { maxOutputTokens: 16, temperature: 0 } }),
      );
      const before = session.turnCount;
      expect(before).toBeGreaterThanOrEqual(1);
      session.undoTurns(1);
      expect(session.turnCount).toBe(before - 1);
    },
    2 * 60_000,
  );

  it("dispose() flips the disposed flag and is idempotent", async () => {
    if (fixture === undefined) throw new Error("fixture missing");
    const oneShot = new ChatSession(fixture.model);
    expect(oneShot.disposed).toBe(false);
    oneShot.dispose();
    expect(oneShot.disposed).toBe(true);
    await expect(oneShot.preflightRequest(new Request())).rejects.toThrow(/disposed/);
    expect(() => oneShot.dispose()).not.toThrow();
  });
});

describe.skipIf(!haveTestModelCache)("ChatSession tool registration", () => {
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

  it("registers a function tool and a custom tool", () => {
    if (session === undefined) throw new Error("fixture missing");
    session.addToolDefinition({
      name: "multiply_numbers",
      description: "Multiplies two numbers.",
      jsonSchema: JSON.stringify({ type: "object", properties: { first: { type: "integer" } } }),
    });
    // A custom tool carries no schema — the one the model sees is synthesized natively.
    expect(session.addCustomToolDefinition({ name: "apply_patch", description: "Applies a patch." })).toBe(session);
    session.addToolDefinition({ name: "run_shell", description: "Runs a command.", kind: "custom" });
  });

  it("rejects a duplicate name across kinds and allows re-adding after removal", () => {
    if (session === undefined) throw new Error("fixture missing");
    session.addCustomToolDefinition({ name: "apply_patch", description: "Applies a patch." });

    expect(() => session?.addCustomToolDefinition({ name: "apply_patch", description: "again" })).toThrow();
    expect(() => session?.addToolDefinition({ name: "apply_patch", description: "d", jsonSchema: "{}" })).toThrow();

    // Names are case-sensitive, so a differently-cased name is a different tool.
    session.addCustomToolDefinition({ name: "Apply_Patch", description: "Different tool." });

    expect(session.removeToolDefinition("apply_patch")).toBe(true);
    expect(session.removeToolDefinition("apply_patch")).toBe(false);
    session.addToolDefinition({ name: "apply_patch", description: "Now a function.", jsonSchema: "{}" });
  });

  it("rejects a custom tool that supplies a schema and a function tool with an invalid schema", () => {
    if (session === undefined) throw new Error("fixture missing");
    expect(() =>
      // @ts-expect-error — exercising the native guard against a schema on a custom tool
      session?.addToolDefinition({ name: "custom", description: "d", jsonSchema: "{}", kind: "custom" }),
    ).toThrow();
    expect(() => session?.addToolDefinition({ name: "broken", description: "d", jsonSchema: "{not json" })).toThrow();
  });

  it("rejects an unknown tool kind", () => {
    if (session === undefined) throw new Error("fixture missing");
    expect(() =>
      session?.addToolDefinition({
        name: "weird",
        description: "d",
        // @ts-expect-error — exercising the native guard against unknown tool kinds
        kind: "grammar",
      }),
    ).toThrow(TypeError);
  });
});

describe("ChatSession constructor type guard", () => {
  it("throws TypeError when constructed with a non-Model argument", () => {
    expect(() => new ChatSession({} as never)).toThrow(TypeError);
    expect(() => new ChatSession({} as never)).toThrow(/Model/);
  });
});

describe("ChatSession.preflightRequest types", () => {
  it("exposes the asynchronous, strongly typed public result", () => {
    expectTypeOf<ChatSession["preflightRequest"]>().returns.toEqualTypeOf<Promise<RequestPreflight>>();
    expectTypeOf<RequestPreflight>().toEqualTypeOf<{
      readonly promptTokens: number;
      readonly outputReserveTokens: number;
      readonly requiredTokens: number;
      readonly contextLimitTokens: number;
      readonly fits: boolean;
      readonly deficitTokens: number;
    }>();
  });
});
