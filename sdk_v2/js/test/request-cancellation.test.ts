import { afterEach, describe, expect, it, vi } from "vitest";

import { FlErrorCode } from "../src/detail/errors.js";
import type { NativeResponse, NativeSession } from "../src/detail/native.js";
import type { Request } from "../src/request.js";
import { Session } from "../src/session.js";

vi.mock("../src/request.js", () => ({
  unwrapNativeRequest: (request: unknown) => request,
}));

class TestSession extends Session {
  static create(native: NativeSession): TestSession {
    return new TestSession(native);
  }
}

function fixture() {
  let resolve!: (response: NativeResponse) => void;
  let reject!: (error: unknown) => void;
  const pending = Object.assign(
    new Promise<NativeResponse>((resolveResponse, rejectResponse) => {
      resolve = resolveResponse;
      reject = rejectResponse;
    }),
    { cancelQueued: vi.fn(() => false) },
  );
  const processRequest = vi.fn(() => pending);
  const native = { processRequest, processStreamingRequest: processRequest } as unknown as NativeSession;
  const cancel = vi.fn();
  const request = { cancel } as unknown as Request;
  return { session: TestSession.create(native), request, pending, processRequest, cancel, resolve, reject };
}

function cancellationError() {
  return Object.assign(new Error("Request canceled"), {
    name: "FoundryLocalError",
    code: FlErrorCode.OperationCancelled,
  });
}

afterEach(() => {
  vi.useRealTimers();
});

describe("non-streaming request cancellation", () => {
  it("rejects a pre-aborted signal without submitting native inference", async () => {
    const test = fixture();
    const controller = new AbortController();
    controller.abort();
    await expect(test.session.processRequest(test.request, { signal: controller.signal })).rejects.toMatchObject({
      name: "AbortError",
    });

    expect(test.processRequest).not.toHaveBeenCalled();
    expect(test.cancel).not.toHaveBeenCalled();
  });

  it("retries cancellation across admission and stops retries when native processing settles", async () => {
    vi.useFakeTimers();
    const test = fixture();
    const controller = new AbortController();
    const removeListener = vi.spyOn(controller.signal, "removeEventListener");
    const pending = test.session.processRequest(test.request, { signal: controller.signal });
    const rejection = expect(pending).rejects.toMatchObject({
      name: "AbortError",
      code: FlErrorCode.OperationCancelled,
    });
    controller.abort();
    await vi.advanceTimersByTimeAsync(20);
    expect(test.cancel.mock.calls.length).toBeGreaterThan(1);
    test.reject(cancellationError());
    await rejection;
    expect(vi.getTimerCount()).toBe(0);
    expect(removeListener).toHaveBeenCalledWith("abort", expect.any(Function));
  });

  it("removes queued work without canceling an idle reusable request", async () => {
    const test = fixture();
    const controller = new AbortController();
    test.pending.cancelQueued.mockImplementation(() => {
      test.reject(cancellationError());
      return true;
    });
    const pending = test.session.processRequest(test.request, { signal: controller.signal });
    controller.abort();
    await expect(pending).rejects.toMatchObject({ name: "AbortError" });
    expect(test.cancel).not.toHaveBeenCalled();
  });

  it("does not relabel unrelated native errors after an abort", async () => {
    const test = fixture();
    const controller = new AbortController();
    const pending = test.session.processRequest(test.request, { signal: controller.signal });
    controller.abort();
    const failure = new Error("invalid request");
    test.reject(failure);
    await expect(pending).rejects.toBe(failure);
  });
});

describe("streaming request cancellation", () => {
  it("cancels without waiting for an output callback and stops admission retries", async () => {
    vi.useFakeTimers();
    const test = fixture();
    const controller = new AbortController();
    const stream = test.session.processStreamingRequest(test.request, { signal: controller.signal });
    const terminal = expect(stream.response).rejects.toMatchObject({ name: "AbortError" });
    controller.abort();
    await vi.advanceTimersByTimeAsync(20);
    expect(test.cancel.mock.calls.length).toBeGreaterThan(1);
    test.reject(cancellationError());
    await terminal;
    await expect(stream[Symbol.asyncIterator]().next()).rejects.toMatchObject({ name: "AbortError" });
    expect(vi.getTimerCount()).toBe(0);
  });
});
