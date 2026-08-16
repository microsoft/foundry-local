import { describe, expect, it, vi } from "vitest";

import { FlErrorCode } from "../src/detail/errors.js";
import type { NativeResponse, NativeSession } from "../src/detail/native.js";
import { Request } from "../src/request.js";
import { Session } from "../src/session.js";

const CONCURRENT_STREAM_ERROR =
  "Streaming cannot overlap another request on the same session. Wait for the active request to settle.";

function deferred<T>() {
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

function fakeNativeSession({
  processRequest = vi.fn(async () => RESPONSE),
  processStreamingRequest = vi.fn(async () => RESPONSE),
}: Partial<Pick<NativeSession, "processRequest" | "processStreamingRequest">> = {}): NativeSession {
  return {
    processRequest,
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
  it("blocks every overlap with an unsettled stream, then releases", async () => {
    const result = deferred<NativeResponse>();
    const processRequest = vi.fn<NativeSession["processRequest"]>().mockResolvedValue(RESPONSE);
    const processStreamingRequest = vi
      .fn<NativeSession["processStreamingRequest"]>()
      .mockReturnValueOnce(result.promise)
      .mockResolvedValue(RESPONSE);
    const session = TestSession.create(fakeNativeSession({ processRequest, processStreamingRequest }));
    const controller = new AbortController();
    const removeListener = vi.spyOn(controller.signal, "removeEventListener");

    const first = session.processStreamingRequest(new Request(), { signal: controller.signal });

    expect(() => session.processStreamingRequest(new Request())).toThrowError(CONCURRENT_STREAM_ERROR);
    await expect(session.processRequest(new Request())).rejects.toThrowError(CONCURRENT_STREAM_ERROR);
    expect(processRequest).not.toHaveBeenCalled();

    result.resolve(RESPONSE);
    await expect(first.response).resolves.toBe(RESPONSE);
    expect(removeListener).toHaveBeenCalledWith("abort", expect.any(Function));

    await expect(session.processRequest(new Request())).resolves.toBe(RESPONSE);
    await expect(session.processStreamingRequest(new Request()).response).resolves.toBe(RESPONSE);
  });

  it("allows overlapping non-streaming calls but blocks a stream until all settle", async () => {
    const firstResult = deferred<NativeResponse>();
    const secondResult = deferred<NativeResponse>();
    const processRequest = vi
      .fn<NativeSession["processRequest"]>()
      .mockReturnValueOnce(firstResult.promise)
      .mockReturnValueOnce(secondResult.promise);
    const processStreamingRequest = vi.fn<NativeSession["processStreamingRequest"]>().mockResolvedValue(RESPONSE);
    const session = TestSession.create(fakeNativeSession({ processRequest, processStreamingRequest }));

    const first = session.processRequest(new Request());
    const second = session.processRequest(new Request());
    expect(processRequest).toHaveBeenCalledTimes(2);
    expect(() => session.processStreamingRequest(new Request())).toThrowError(CONCURRENT_STREAM_ERROR);

    firstResult.resolve(RESPONSE);
    await expect(first).resolves.toBe(RESPONSE);
    expect(() => session.processStreamingRequest(new Request())).toThrowError(CONCURRENT_STREAM_ERROR);

    secondResult.resolve(RESPONSE);
    await expect(second).resolves.toBe(RESPONSE);
    await expect(session.processStreamingRequest(new Request()).response).resolves.toBe(RESPONSE);
  });

  it.each(["native rejection", "synchronous startup throw", "pre-abort", "abort"] as const)(
    "releases after %s",
    async (scenario) => {
      const processStreamingRequest = vi.fn<NativeSession["processStreamingRequest"]>().mockResolvedValue(RESPONSE);
      const session = TestSession.create(fakeNativeSession({ processStreamingRequest }));

      if (scenario === "native rejection") {
        const failure = new Error("native failure");
        processStreamingRequest.mockRejectedValueOnce(failure);
        await expect(session.processStreamingRequest(new Request()).response).rejects.toBe(failure);
      } else if (scenario === "synchronous startup throw") {
        const failure = new Error("startup failed");
        processStreamingRequest.mockImplementationOnce(() => {
          throw failure;
        });
        expect(() => session.processStreamingRequest(new Request())).toThrow(failure);
      } else if (scenario === "pre-abort") {
        const controller = new AbortController();
        controller.abort();
        await expect(
          session.processStreamingRequest(new Request(), { signal: controller.signal }).response,
        ).rejects.toMatchObject({ name: "AbortError" });
      } else {
        const nativeResult = deferred<NativeResponse>();
        processStreamingRequest.mockReturnValueOnce(nativeResult.promise);
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
      }

      const callsBeforeRetry = processStreamingRequest.mock.calls.length;
      await expect(session.processStreamingRequest(new Request()).response).resolves.toBe(RESPONSE);
      expect(processStreamingRequest).toHaveBeenCalledTimes(callsBeforeRetry + 1);
    },
  );
});
