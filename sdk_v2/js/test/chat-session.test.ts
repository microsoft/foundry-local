// Non-streaming ChatSession tests against a real loaded chat model.
// Gated by FOUNDRY_TEST_DATA_DIR. Streaming + AbortSignal coverage lives in
// streaming.test.ts.
import { afterAll, afterEach, beforeAll, beforeEach, describe, expect, it } from "vitest";

import { Item } from "../src/items.js";
import { Request } from "../src/request.js";
import { ChatSession, Session } from "../src/session.js";

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

  it("dispose() flips the disposed flag and is idempotent", () => {
    if (fixture === undefined) throw new Error("fixture missing");
    const oneShot = new ChatSession(fixture.model);
    expect(oneShot.disposed).toBe(false);
    oneShot.dispose();
    expect(oneShot.disposed).toBe(true);
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
