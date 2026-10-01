import { afterAll, afterEach, beforeAll, beforeEach, describe, expect, it } from "vitest";

import type { NativeGenericSession } from "../src/detail/native.js";
import { Item } from "../src/items.js";
import { Request, unwrapNativeRequest } from "../src/request.js";
import { DecisionSession, RankingSession, TypedDecisionSession } from "../src/session.js";

import {
  type CacheOnlyManagerFixture,
  haveNativePrereqs,
  setupCacheOnlyManager,
  teardownCacheOnlyManager,
} from "./_fixtures/cacheOnlyManager.js";
import {
  type RealModelManagerFixture,
  SkipFixture,
  haveTestModelCache,
  setupRealModelManager,
  teardownRealModelManager,
  testModelCacheDiagnostic,
} from "./_fixtures/realModelManager.js";

if (!haveTestModelCache) {
  console.warn(testModelCacheDiagnostic);
}

describe("non-generative session constructor guards", () => {
  it("requires a Model instance", () => {
    expect(() => new RankingSession({} as never)).toThrow(TypeError);
    expect(() => new RankingSession({} as never)).toThrow(/Model/);
    expect(TypedDecisionSession).toBe(DecisionSession);
    expect(() => new DecisionSession({} as never)).toThrow(TypeError);
    expect(() => new DecisionSession({} as never)).toThrow(/Model/);
  });
});

describe.skipIf(!haveNativePrereqs)("non-generative session task validation", () => {
  let fixture: CacheOnlyManagerFixture | undefined;

  beforeAll(() => {
    fixture = setupCacheOnlyManager({ appName: "foundry-local-js-sdk-v2-non-generative-wrong-task" });
  });

  afterAll(() => {
    if (fixture !== undefined) teardownCacheOnlyManager(fixture);
  });

  it("rejects models with the wrong task before native construction", async () => {
    if (fixture === undefined) throw new Error("fixture missing");
    const chatModel = await fixture.manager.catalog.getModel("phi-4-mini-instruct");
    expect(() => new RankingSession(chatModel)).toThrow(/text-ranking.*chat-completion/);
    expect(() => new DecisionSession(chatModel)).toThrow(/typed-decision.*chat-completion/);
  });
});

describe.skipIf(!haveTestModelCache)("RankingSession (real model)", () => {
  let fixture: RealModelManagerFixture | undefined;
  let session: RankingSession | undefined;
  let skipReason: string | undefined;

  beforeAll(async () => {
    try {
      fixture = await setupRealModelManager({
        task: "text-ranking",
        namePreference: "clm",
        loadModel: false,
      });
    } catch (error) {
      if (!(error instanceof SkipFixture)) throw error;
      skipReason = error.message;
    }
  }, 5 * 60_000);

  afterAll(() => teardownRealModelManager(fixture));

  beforeEach(() => {
    if (fixture === undefined) return;
    session = new RankingSession(fixture.model);
  });

  afterEach(() => {
    session?.dispose();
    session = undefined;
  });

  it("ranks candidates with the typed helper", async (context) => {
    if (skipReason !== undefined) context.skip();
    if (session === undefined) throw new Error("session missing");
    const result = await session.rank({
      context: { weather: "heavy rain" },
      question: "Which activity is more suitable?",
      answers: ["Have a picnic outdoors", "Visit an indoor museum"],
    });

    expect(result.model).toBeTruthy();
    expect(result.ranked).toHaveLength(2);
    expect(result.ranked.map((candidate) => candidate.candidate).sort()).toEqual(
      ["Have a picnic outdoors", "Visit an indoor museum"].sort(),
    );
  });

  it("uses one OpenAI JSON text item at the low-level boundary", async (context) => {
    if (skipReason !== undefined) context.skip();
    if (session === undefined) throw new Error("session missing");
    const request = new Request().addItem(Item.text(JSON.stringify({ answers: ["first", "second"] }), "openai-json"));
    const response = await session.processRequest(request);
    expect(response.output).toHaveLength(1);
    expect(response.output[0]).toMatchObject({ type: "text", textType: "openai-json" });
  });

  it("serializes concurrent requests before occupying another worker", async (context) => {
    if (skipReason !== undefined) context.skip();
    if (session === undefined) throw new Error("session missing");
    const nativeSession = (session as unknown as { native: NativeGenericSession }).native;
    const request = () =>
      new Request().addItem(Item.text(JSON.stringify({ answers: ["first", "second"] }), "openai-json"));

    let signalFirstStarted!: (release: () => void) => void;
    const firstStarted = new Promise<() => void>((resolve) => {
      signalFirstStarted = resolve;
    });
    const firstRequest = request();
    const first = nativeSession.processRequest(unwrapNativeRequest(firstRequest), signalFirstStarted);
    const releaseFirst = await firstStarted;

    let secondStarted = false;
    let signalSecondStarted!: (release: () => void) => void;
    const secondStartedPromise = new Promise<() => void>((resolve) => {
      signalSecondStarted = (release) => {
        secondStarted = true;
        resolve(release);
      };
    });
    const secondRequest = request();
    const second = nativeSession.processRequest(unwrapNativeRequest(secondRequest), signalSecondStarted);
    await new Promise<void>((resolve) => setImmediate(resolve));
    expect(secondStarted).toBe(false);

    releaseFirst();
    await expect(first).resolves.toMatchObject({ output: expect.any(Array) });
    const releaseSecond = await secondStartedPromise;
    releaseSecond();
    await expect(second).resolves.toMatchObject({ output: expect.any(Array) });
  });

  it("does not expose streaming on one-shot predictive sessions", (context) => {
    if (skipReason !== undefined) context.skip();
    if (session === undefined) throw new Error("session missing");
    expect("processStreamingRequest" in session).toBe(false);
  });

  it("disposes idempotently", (context) => {
    if (skipReason !== undefined) context.skip();
    session?.dispose();
    expect(session?.disposed).toBe(true);
    expect(() => session?.dispose()).not.toThrow();
  });
});

describe.skipIf(!haveTestModelCache)("DecisionSession (real model)", () => {
  let fixture: RealModelManagerFixture | undefined;
  let session: DecisionSession | undefined;
  let skipReason: string | undefined;

  beforeAll(async () => {
    try {
      fixture = await setupRealModelManager({
        task: "typed-decision",
        namePreference: "kev",
        loadModel: false,
      });
    } catch (error) {
      if (!(error instanceof SkipFixture)) throw error;
      skipReason = error.message;
    }
  }, 5 * 60_000);

  afterAll(() => teardownRealModelManager(fixture));

  beforeEach(() => {
    if (fixture === undefined) return;
    session = new DecisionSession(fixture.model);
  });

  afterEach(() => {
    session?.dispose();
    session = undefined;
  });

  it("returns typed answers with the typed helper", async (context) => {
    if (skipReason !== undefined) context.skip();
    if (session === undefined) throw new Error("session missing");
    const result = await session.decide({
      state: { weather: "heavy rain" },
      questions: {
        umbrella: { type: "noul", instructions: "Should I take an umbrella?" },
      },
    });

    expect(result.model).toBeTruthy();
    expect(result.answers.umbrella?.type).toBe("noul");
    expect(result.usage.billing_units).toBe(0);
  });
});
