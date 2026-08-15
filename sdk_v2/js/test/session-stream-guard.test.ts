import { describe, expect, it, vi } from "vitest";

import { FlErrorCode } from "../src/detail/errors.js";
import type { NativeResponse, NativeSession } from "../src/detail/native.js";
import { Request } from "../src/request.js";
import { Session } from "../src/session.js";

const CONCURRENT_STREAM_ERROR =
  "Concurrent streaming requests on the same session are not supported. " +
  "Drain or cancel the in-flight stream before starting another.";

type Deferred<T> = {
  readonly promise: Promise<T>;
  readonly resolve: (value: T) => void;
  readonly reject: (reason: unknown) => void;
};

function deferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  let reject!: (reason: unknown) => void;
  const promise = new Promise<T>((resolvePromise, rejectPromise) => {
    resolve = resolvePromise;
    reject = rejectPromise;
  });
  return { promise, resolve, reject };
}

const RESPONSE: NativeResponse = {
  output: [],
  finishReason: "stop",
  usage: { promptTokens: 1, completionTokens: 1, totalTokens: 2 },
};

function fakeNativeSession(processStreamingRequest: NativeSession["processStreamingRequest"]): NativeSession {
  return {
    processRequest: vi.fn(async () => RESPONSE),
    processStreamingRequest,
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

describe("Session streaming concurrency guard", () => {
  it("rejects overlap while an eager unconsumed stream is unsettled and releases after success", async () => {
    const firstResult = deferred<NativeResponse>();
    const processStreamingRequest = vi
      .fn<NativeSession["processStreamingRequest"]>()
      .mockReturnValueOnce(firstResult.promise)
      .mockResolvedValue(RESPONSE);
    const session = TestSession.create(fakeNativeSession(processStreamingRequest));

    const first = session.processStreamingRequest(new Request());

    expect(() => session.processStreamingRequest(new Request())).toThrowError(CONCURRENT_STREAM_ERROR);
    expect(processStreamingRequest).toHaveBeenCalledTimes(1);

    firstResult.resolve(RESPONSE);
    await expect(first.response).resolves.toBe(RESPONSE);

    const next = session.processStreamingRequest(new Request());
    await expect(next.response).resolves.toBe(RESPONSE);
    expect(processStreamingRequest).toHaveBeenCalledTimes(2);
  });

  it("releases the guard after native rejection", async () => {
    const failure = new Error("native failure");
    const processStreamingRequest = vi
      .fn<NativeSession["processStreamingRequest"]>()
      .mockRejectedValueOnce(failure)
      .mockResolvedValue(RESPONSE);
    const session = TestSession.create(fakeNativeSession(processStreamingRequest));

    const failed = session.processStreamingRequest(new Request());
    await expect(failed.response).rejects.toBe(failure);

    await expect(session.processStreamingRequest(new Request()).response).resolves.toBe(RESPONSE);
    expect(processStreamingRequest).toHaveBeenCalledTimes(2);
  });

  it("keeps the guard released when a consumer breaks after native settlement", async () => {
    const processStreamingRequest = vi
      .fn<NativeSession["processStreamingRequest"]>()
      .mockImplementationOnce(async (_request, onItem) => {
        onItem({ type: "text", text: "token", kind: "complete" });
        return RESPONSE;
      })
      .mockResolvedValue(RESPONSE);
    const session = TestSession.create(fakeNativeSession(processStreamingRequest));
    const request = new Request();
    const cancel = vi.spyOn(request, "cancel");
    const stream = session.processStreamingRequest(request);
    await stream.response;

    for await (const _item of stream) {
      break;
    }

    expect(cancel).not.toHaveBeenCalled();
    await expect(session.processStreamingRequest(new Request()).response).resolves.toBe(RESPONSE);
    expect(processStreamingRequest).toHaveBeenCalledTimes(2);
  });

  it("releases the guard after abort settlement", async () => {
    const nativeResult = deferred<NativeResponse>();
    const processStreamingRequest = vi
      .fn<NativeSession["processStreamingRequest"]>()
      .mockReturnValueOnce(nativeResult.promise)
      .mockResolvedValue(RESPONSE);
    const session = TestSession.create(fakeNativeSession(processStreamingRequest));
    const request = new Request();
    const controller = new AbortController();
    vi.spyOn(request, "cancel").mockImplementation(() => {
      nativeResult.reject(
        Object.assign(new Error("cancelled"), {
          name: "FoundryLocalError",
          code: FlErrorCode.OperationCancelled,
        }),
      );
    });

    const aborted = session.processStreamingRequest(request, { signal: controller.signal });
    controller.abort();
    await expect(aborted.response).rejects.toMatchObject({ name: "AbortError" });

    await expect(session.processStreamingRequest(new Request()).response).resolves.toBe(RESPONSE);
    expect(processStreamingRequest).toHaveBeenCalledTimes(2);
  });

  it("releases the guard when native startup throws synchronously", async () => {
    const failure = new Error("startup failed");
    const processStreamingRequest = vi
      .fn<NativeSession["processStreamingRequest"]>()
      .mockImplementationOnce(() => {
        throw failure;
      })
      .mockResolvedValue(RESPONSE);
    const session = TestSession.create(fakeNativeSession(processStreamingRequest));

    expect(() => session.processStreamingRequest(new Request())).toThrow(failure);

    await expect(session.processStreamingRequest(new Request()).response).resolves.toBe(RESPONSE);
    expect(processStreamingRequest).toHaveBeenCalledTimes(2);
  });

  it("does not retain the guard for a pre-aborted stream that starts no native work", async () => {
    const processStreamingRequest = vi.fn<NativeSession["processStreamingRequest"]>().mockResolvedValue(RESPONSE);
    const session = TestSession.create(fakeNativeSession(processStreamingRequest));
    const controller = new AbortController();
    controller.abort();

    const aborted = session.processStreamingRequest(new Request(), { signal: controller.signal });
    const next = session.processStreamingRequest(new Request());

    await expect(aborted.response).rejects.toMatchObject({ name: "AbortError" });
    await expect(next.response).resolves.toBe(RESPONSE);
    expect(processStreamingRequest).toHaveBeenCalledTimes(1);
  });
});
