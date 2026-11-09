import { describe, expect, it, vi } from "vitest";

import { FlErrorCode } from "../src/detail/errors.js";
import type { NativeOneShotSession, NativeResponse } from "../src/detail/native.js";
import { DecisionSession, RankingSession } from "../src/session.js";

const mocks = vi.hoisted(() => ({
  requests: [] as Array<{ cancel: ReturnType<typeof vi.fn> }>,
}));

vi.mock("../src/request.js", () => ({
  Request: class {
    readonly cancel = vi.fn();

    constructor() {
      mocks.requests.push(this);
    }

    addItem(): this {
      return this;
    }
  },
  unwrapNativeRequest: (request: unknown) => request,
}));

vi.mock("../src/items.js", () => ({
  Item: { text: (text: string, textType: string) => ({ type: "text", text, textType }) },
}));

function response(value: unknown): NativeResponse {
  return {
    output: [{ type: "text", textType: "openai-json", text: JSON.stringify(value) }],
    finishReason: "stop",
    usage: { promptTokens: 0, completionTokens: 0, totalTokens: 0 },
  };
}

function pendingResponse() {
  let resolve!: (value: NativeResponse) => void;
  let reject!: (reason: unknown) => void;
  const promise = Object.assign(
    new Promise<NativeResponse>((resolvePromise, rejectPromise) => {
      resolve = resolvePromise;
      reject = rejectPromise;
    }),
    { cancelQueued: vi.fn(() => false) },
  );
  return { promise, resolve, reject };
}

function rankingSession(native: NativeOneShotSession): RankingSession {
  const session = Object.create(RankingSession.prototype) as RankingSession;
  Object.defineProperty(session, "native", { value: native });
  return session;
}

function decisionSession(native: NativeOneShotSession): DecisionSession {
  const session = Object.create(DecisionSession.prototype) as DecisionSession;
  Object.defineProperty(session, "native", { value: native });
  return session;
}

function immediateNative(value: unknown): NativeOneShotSession {
  return {
    processRequest: vi.fn(() =>
      Object.assign(Promise.resolve(response(value)), {
        cancelQueued: () => false,
      }),
    ),
    setOptions: vi.fn(),
    dispose: vi.fn(),
    isDisposed: vi.fn(() => false),
  };
}

const rankingRequest = { answers: ["first", "second"] } as const;
const rankingValue = {
  model: "clm:1",
  ranked: [
    { rank: 1, id: "1", index: 1, candidate: "second", prob: 0.8 },
    { rank: 2, id: "0", index: 0, candidate: "first", prob: 0.2 },
  ],
};

describe("typed predictive request validation", () => {
  it.each([
    [{ answers: [] }, /answers/],
    [{ answers: [""] }, /answers/],
    [{ answers: ["ok"], question: 1 }, /question/],
    [{ answers: ["ok"], context: new Date() }, /plain JSON objects/],
    [{ answers: ["ok"], context: { value: Number.NaN } }, /finite/],
    [{ answers: ["ok"], temperature: 0 }, /temperature/],
    [{ answers: Array(1) }, /answers/],
    [{ answers: ["ok"], context: Array(1) }, /JSON-compatible/],
  ])("rejects an invalid ranking request before native submission", async (request, message) => {
    const native = immediateNative(rankingValue);
    await expect(rankingSession(native).rank(request as never)).rejects.toThrow(message);
    expect(native.processRequest).not.toHaveBeenCalled();
  });

  it.each([
    [{ questions: {} }, /questions/],
    [{ questions: { q: { type: "unknown" } } }, /type/],
    [{ questions: { q: { type: "choice" } } }, /criteria/],
    [{ questions: { q: { type: "score", criteria: {} } } }, /criteria/],
    [{ questions: { q: { type: "noul", criteria: [] } } }, /not supported/],
    [{ questions: { q: { type: "noul", instructions: new Map() } } }, /plain JSON objects/],
    [{ questions: { q: { type: "noul" } }, temperature: 2 }, /exactly 1.0/],
  ])("rejects an invalid decision request before native submission", async (request, message) => {
    const native = immediateNative({});
    await expect(decisionSession(native).decide(request as never)).rejects.toThrow(message);
    expect(native.processRequest).not.toHaveBeenCalled();
  });

  it.each([
    { questions: { q: { type: "noul" } } },
    { questions: { q: { type: "noul", instructions: undefined } } },
    { questions: { q: { type: "noul", criteria: null } } },
  ])("accepts omitted, undefined, and nullable noul options", async (request) => {
    const value = {
      model: "kev:1",
      answers: { q: { type: "noul", noul: 0.5 } },
      usage: { billing_units: 0 },
    };
    await expect(decisionSession(immediateNative(value)).decide(request as never)).resolves.toEqual(value);
  });
});

describe("typed predictive result validation", () => {
  it("accepts a complete ranking permutation", async () => {
    await expect(rankingSession(immediateNative(rankingValue)).rank(rankingRequest)).resolves.toEqual(rankingValue);
  });

  it.each([
    { ...rankingValue, ranked: rankingValue.ranked.slice(0, 1) },
    { ...rankingValue, ranked: rankingValue.ranked.map((item) => ({ ...item, rank: 1 })) },
    { ...rankingValue, ranked: rankingValue.ranked.map((item) => ({ ...item, index: 0, id: "0" })) },
    { ...rankingValue, ranked: [{ ...rankingValue.ranked[0], rank: 3 }, rankingValue.ranked[1]] },
    { ...rankingValue, ranked: [{ ...rankingValue.ranked[0], prob: Number.NaN }, rankingValue.ranked[1]] },
    { ...rankingValue, ranked: [{ ...rankingValue.ranked[0], prob: 1.1 }, rankingValue.ranked[1]] },
    { ...rankingValue, ranked: [{ ...rankingValue.ranked[0], candidate: "unknown" }, rankingValue.ranked[1]] },
  ])("rejects malformed ranking results", async (value) => {
    await expect(rankingSession(immediateNative(value)).rank(rankingRequest)).rejects.toThrow(/typed result/);
  });

  it("accepts answers whose keys and discriminants match the request", async () => {
    const request = {
      questions: {
        selected: { type: "choice", criteria: { inside: "Inside" } },
        confidence: { type: "score", criteria: ["low", "high"] },
      },
    } as const;
    const value = {
      model: "kev:1",
      answers: {
        selected: { type: "choice", choice: "inside", probabilities: { inside: 0.8 } },
        confidence: { type: "score", score: 1, confidence: 0.9 },
      },
      usage: { billing_units: 0 },
    };
    await expect(decisionSession(immediateNative(value)).decide(request)).resolves.toEqual(value);
  });

  it.each([
    { model: "kev:1", answers: {}, usage: { billing_units: 0 } },
    { model: "kev:1", answers: { q: { type: "score", score: 1 } }, usage: { billing_units: 0 } },
    { model: "kev:1", answers: { q: { type: "noul", noul: Number.NaN } }, usage: { billing_units: 0 } },
    { model: "kev:1", answers: { q: { type: "noul", noul: -0.1 } }, usage: { billing_units: 0 } },
    { model: "kev:1", answers: { q: { type: "noul", noul: 1.1 } }, usage: { billing_units: 0 } },
    {
      model: "kev:1",
      answers: { q: { type: "noul", noul: 0.5, choice: "extra" } },
      usage: { billing_units: 0 },
    },
    {
      model: "kev:1",
      answers: { q: { type: "noul", noul: 0.5, confidence: 1.1 } },
      usage: { billing_units: 0 },
    },
    {
      model: "kev:1",
      answers: { q: { type: "noul", noul: 0.5, probabilities: { yes: -0.1 } } },
      usage: { billing_units: 0 },
    },
    { model: "kev:1", answers: { q: { type: "noul", noul: 1 } }, usage: { billing_units: 0.5 } },
  ])("rejects malformed decision results", async (value) => {
    const request = { questions: { q: { type: "noul" } } } as const;
    await expect(decisionSession(immediateNative(value)).decide(request)).rejects.toThrow(/typed result/);
  });

  it.each([
    [
      { type: "choice", criteria: { alpha: "A", omega: "Z" } },
      { type: "choice", choice: "missing" },
    ],
    [
      { type: "score", criteria: ["low", "high"] },
      { type: "score", score: -0.1 },
    ],
    [
      { type: "score", criteria: ["low", "high"] },
      { type: "score", score: 1.1 },
    ],
    [
      { type: "score", criteria: ["low", "high"] },
      { type: "score", score: 0.5, noul: 0.5 },
    ],
  ])("rejects answers outside captured question criteria", async (question, answer) => {
    const value = { model: "kev:1", answers: { q: answer }, usage: { billing_units: 0 } };
    await expect(
      decisionSession(immediateNative(value)).decide({ questions: { q: question } } as never),
    ).rejects.toThrow(/typed result/);
  });

  it("accepts a fractional score inside the captured range", async () => {
    const value = {
      model: "kev:1",
      answers: { q: { type: "score", score: 0.5 } },
      usage: { billing_units: 0 },
    };
    await expect(
      decisionSession(immediateNative(value)).decide({
        questions: { q: { type: "score", criteria: ["low", "high"] } },
      }),
    ).resolves.toEqual(value);
  });

  it("validates a ranking response against candidates captured before dispatch", async () => {
    const pending = pendingResponse();
    const native = immediateNative(rankingValue);
    vi.mocked(native.processRequest).mockReturnValueOnce(pending.promise);
    const answers = ["first", "second"];
    const result = rankingSession(native).rank({ answers });
    answers[0] = "mutated";
    answers.push("third");
    pending.resolve(response(rankingValue));
    await expect(result).resolves.toEqual(rankingValue);
  });

  it("validates a decision response against questions captured before dispatch", async () => {
    const pending = pendingResponse();
    const native = immediateNative({});
    vi.mocked(native.processRequest).mockReturnValueOnce(pending.promise);
    const questions = {
      selected: { type: "choice" as const, criteria: { nonnumeric: "Inside" } },
      score: { type: "score" as const, criteria: ["low", "high"] },
    };
    const value = {
      model: "kev:1",
      answers: {
        selected: { type: "choice", choice: "nonnumeric" },
        score: { type: "score", score: 0.5 },
      },
      usage: { billing_units: 0 },
    };
    const result = decisionSession(native).decide({ questions });
    questions.selected.type = "score" as never;
    const mutatedCriteria = questions.selected.criteria as Record<string, string>;
    Reflect.deleteProperty(mutatedCriteria, "nonnumeric");
    mutatedCriteria.changed = "Changed";
    questions.score.criteria.length = 1;
    const mutatedQuestions = questions as unknown as Record<string, unknown>;
    Reflect.deleteProperty(mutatedQuestions, "score");
    mutatedQuestions.replacement = { type: "noul" };
    pending.resolve(response(value));
    await expect(result).resolves.toEqual(value);
  });
});

describe("typed predictive cancellation and lifecycle", () => {
  it("does not submit a pre-aborted ranking request", async () => {
    const native = immediateNative(rankingValue);
    const controller = new AbortController();
    controller.abort();
    await expect(rankingSession(native).rank(rankingRequest, { signal: controller.signal })).rejects.toMatchObject({
      name: "AbortError",
    });
    expect(native.processRequest).not.toHaveBeenCalled();
  });

  it("removes a deterministically queued typed request", async () => {
    const pending = pendingResponse();
    const native = immediateNative(rankingValue);
    vi.mocked(native.processRequest).mockReturnValueOnce(pending.promise);
    pending.promise.cancelQueued.mockImplementation(() => {
      pending.reject(
        Object.assign(new Error("cancelled"), {
          name: "FoundryLocalError",
          code: FlErrorCode.OperationCancelled,
        }),
      );
      return true;
    });
    const controller = new AbortController();
    const result = rankingSession(native).rank(rankingRequest, { signal: controller.signal });
    controller.abort();
    await expect(result).rejects.toMatchObject({ name: "AbortError" });
    expect(mocks.requests.at(-1)?.cancel).not.toHaveBeenCalled();
  });

  it("cancels a deterministically active typed request", async () => {
    const pending = pendingResponse();
    const native = immediateNative(rankingValue);
    vi.mocked(native.processRequest).mockReturnValueOnce(pending.promise);
    const controller = new AbortController();
    const result = rankingSession(native).rank(rankingRequest, { signal: controller.signal });
    controller.abort();
    expect(mocks.requests.at(-1)?.cancel).toHaveBeenCalledOnce();
    pending.reject(
      Object.assign(new Error("cancelled"), {
        name: "FoundryLocalError",
        code: FlErrorCode.OperationCancelled,
      }),
    );
    await expect(result).rejects.toMatchObject({ name: "AbortError" });
  });

  it("rejects calls after disposal", async () => {
    let disposed = false;
    const native = immediateNative(rankingValue);
    native.dispose = vi.fn(() => {
      disposed = true;
    });
    native.isDisposed = vi.fn(() => disposed);
    vi.mocked(native.processRequest).mockImplementation(() => {
      if (disposed) {
        return Object.assign(
          Promise.reject(
            Object.assign(new Error("disposed"), {
              name: "FoundryLocalError",
              code: FlErrorCode.InvalidUsage,
            }),
          ),
          { cancelQueued: () => false },
        );
      }
      return Object.assign(Promise.resolve(response(rankingValue)), { cancelQueued: () => false });
    });
    const session = rankingSession(native);
    session.dispose();
    await expect(session.rank(rankingRequest)).rejects.toMatchObject({ code: FlErrorCode.InvalidUsage });
  });
});
