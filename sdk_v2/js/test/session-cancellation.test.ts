import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";

import { FlErrorCode } from "../src/detail/errors.js";
import type { NativeResponse, NativeSession } from "../src/detail/native.js";
import { Request } from "../src/request.js";
import { Session } from "../src/session.js";

type Rejectable<T> = {
  readonly promise: Promise<T>;
  readonly reject: (reason: unknown) => void;
};

function rejectable<T>(): Rejectable<T> {
  let reject!: (reason: unknown) => void;
  const promise = new Promise<T>((_resolve, rejectPromise) => {
    reject = rejectPromise;
  });
  return { promise, reject };
}

function operationCancelledError(): Error & { readonly code: number } {
  return Object.assign(new Error("cancelled"), {
    name: "FoundryLocalError",
    code: FlErrorCode.OperationCancelled,
  });
}

function fakeNativeSession(result: Promise<NativeResponse>): NativeSession {
  return {
    processRequest: vi.fn(() => result),
    processStreamingRequest: vi.fn(() => result),
    setOptions: vi.fn(),
    cancel: vi.fn(),
    dispose: vi.fn(),
    isDisposed: vi.fn(() => false),
  };
}

class TestSession extends Session {
  static create(native: NativeSession): TestSession {
    return new TestSession(native);
  }
}

describe("Session AbortSignal cancellation races", () => {
  beforeEach(() => {
    vi.useFakeTimers();
  });

  afterEach(() => {
    vi.restoreAllMocks();
    vi.useRealTimers();
  });

  it("retries non-streaming cancellation after native processing attaches and stops at settlement", async () => {
    const nativeResult = rejectable<NativeResponse>();
    const session = TestSession.create(fakeNativeSession(nativeResult.promise));
    const request = new Request();
    const controller = new AbortController();
    const removeListener = vi.spyOn(controller.signal, "removeEventListener");
    let nativeCancellationAttached = false;
    const cancel = vi.spyOn(request, "cancel").mockImplementation(() => {
      if (nativeCancellationAttached) {
        nativeResult.reject(operationCancelledError());
      }
    });

    const pending = session.processRequest(request, { signal: controller.signal });
    const rejection = expect(pending).rejects.toMatchObject({ name: "AbortError" });

    controller.abort();
    expect(cancel).toHaveBeenCalledTimes(1);
    expect(vi.getTimerCount()).toBe(1);

    nativeCancellationAttached = true;
    await vi.advanceTimersByTimeAsync(50);
    await rejection;

    expect(cancel).toHaveBeenCalledTimes(2);
    expect(removeListener).toHaveBeenCalledWith("abort", expect.any(Function));
    expect(vi.getTimerCount()).toBe(0);

    await vi.advanceTimersByTimeAsync(200);
    expect(cancel).toHaveBeenCalledTimes(2);
  });

  it("retries streaming cancellation after native processing attaches and stops at settlement", async () => {
    const nativeResult = rejectable<NativeResponse>();
    const session = TestSession.create(fakeNativeSession(nativeResult.promise));
    const request = new Request();
    const controller = new AbortController();
    const removeListener = vi.spyOn(controller.signal, "removeEventListener");
    let nativeCancellationAttached = false;
    const cancel = vi.spyOn(request, "cancel").mockImplementation(() => {
      if (nativeCancellationAttached) {
        nativeResult.reject(operationCancelledError());
      }
    });

    const stream = session.processStreamingRequest(request, { signal: controller.signal });
    const iteration = (async (): Promise<void> => {
      for await (const _item of stream) {
        // No items are emitted by this fake native invocation.
      }
    })();
    const responseRejection = expect(stream.response).rejects.toMatchObject({ name: "AbortError" });
    const iterationRejection = expect(iteration).rejects.toMatchObject({ name: "AbortError" });

    controller.abort();
    expect(cancel).toHaveBeenCalledTimes(1);
    expect(vi.getTimerCount()).toBe(1);

    nativeCancellationAttached = true;
    await vi.advanceTimersByTimeAsync(50);
    await Promise.all([responseRejection, iterationRejection]);

    expect(cancel).toHaveBeenCalledTimes(2);
    expect(removeListener).toHaveBeenCalledWith("abort", expect.any(Function));
    expect(vi.getTimerCount()).toBe(0);

    await vi.advanceTimersByTimeAsync(200);
    expect(cancel).toHaveBeenCalledTimes(2);
  });

  it("catches an abort during listener registration and cleans up cancellation retries", async () => {
    const nativeResult = rejectable<NativeResponse>();
    const native = fakeNativeSession(nativeResult.promise);
    const session = TestSession.create(native);
    const request = new Request();
    const controller = new AbortController();
    const originalAddEventListener = controller.signal.addEventListener.bind(controller.signal);
    const removeListener = vi.spyOn(controller.signal, "removeEventListener");
    let nativeCancellationAttached = false;
    const cancel = vi.spyOn(request, "cancel").mockImplementation(() => {
      if (nativeCancellationAttached) {
        nativeResult.reject(operationCancelledError());
      }
    });
    vi.spyOn(controller.signal, "addEventListener").mockImplementation((type, listener, options) => {
      controller.abort();
      originalAddEventListener(type, listener, options);
    });

    const pending = session.processRequest(request, { signal: controller.signal });
    const rejection = expect(pending).rejects.toMatchObject({ name: "AbortError" });

    expect(native.processRequest).toHaveBeenCalledTimes(1);
    expect(cancel).toHaveBeenCalledTimes(1);
    expect(vi.getTimerCount()).toBe(1);

    nativeCancellationAttached = true;
    await vi.advanceTimersByTimeAsync(50);
    await rejection;

    expect(cancel).toHaveBeenCalledTimes(2);
    expect(removeListener).toHaveBeenCalledWith("abort", expect.any(Function));
    expect(vi.getTimerCount()).toBe(0);

    await vi.advanceTimersByTimeAsync(200);
    expect(cancel).toHaveBeenCalledTimes(2);
  });

  it("does not start native work or cancellation retries for pre-aborted signals", async () => {
    const nativeResult = rejectable<NativeResponse>();
    const native = fakeNativeSession(nativeResult.promise);
    const session = TestSession.create(native);
    const request = new Request();
    const cancel = vi.spyOn(request, "cancel");
    const controller = new AbortController();
    controller.abort();

    await expect(session.processRequest(request, { signal: controller.signal })).rejects.toMatchObject({
      name: "AbortError",
    });
    const stream = session.processStreamingRequest(request, { signal: controller.signal });
    await expect(stream.response).rejects.toMatchObject({ name: "AbortError" });

    expect(native.processRequest).not.toHaveBeenCalled();
    expect(native.processStreamingRequest).not.toHaveBeenCalled();
    expect(cancel).not.toHaveBeenCalled();
    expect(vi.getTimerCount()).toBe(0);
  });
});
