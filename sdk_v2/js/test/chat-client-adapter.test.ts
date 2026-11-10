import { afterEach, describe, expect, it, vi } from "vitest";

import { Item } from "../src/items.js";
import type { Model } from "../src/model.js";
import { ChatClient } from "../src/openai/chatClient.js";
import type { Response } from "../src/response.js";
import type { ChatSession } from "../src/session.js";

const { processRequest, dispose } = vi.hoisted(() => ({
  processRequest: vi.fn<ChatSession["processRequest"]>(),
  dispose: vi.fn(),
}));

vi.mock("../src/request.js", () => ({
  Request: class {
    addItem(): this {
      return this;
    }
  },
}));

vi.mock("../src/session.js", () => ({
  ChatSession: class {
    processRequest = processRequest;
    dispose = dispose;
  },
}));

describe("ChatClient cancellation adapter", () => {
  afterEach(() => {
    vi.resetAllMocks();
  });

  it.each([false, true])(
    "preserves the session outcome after abort and permits a subsequent call (cancelled=%s)",
    async (cancelled) => {
      const client = new ChatClient({ id: "adapter-test-model" } as Model);
      const controller = new AbortController();
      let resolve!: (response: Response) => void;
      let reject!: (error: Error) => void;
      processRequest.mockReturnValueOnce(
        new Promise<Response>((resolveResponse, rejectResponse) => {
          resolve = resolveResponse;
          reject = rejectResponse;
        }),
      );
      const completion = { choices: [{ message: { role: "assistant", content: "Hello." } }] };
      const response: Response = {
        output: [Item.text(JSON.stringify(completion), "openai-json")],
        finishReason: "stop",
        usage: { promptTokens: 1, completionTokens: 1, totalTokens: 2 },
      };
      const abortError = Object.assign(new Error("Request cancelled"), { name: "AbortError" });
      const pending = client.completeChat([{ role: "user", content: "Say hello." }], undefined, {
        signal: controller.signal,
      });
      const outcome = cancelled
        ? expect(pending).rejects.toBe(abortError)
        : expect(pending).resolves.toEqual(completion);
      expect(processRequest).toHaveBeenCalledWith(expect.anything(), { signal: controller.signal });
      expect(dispose).not.toHaveBeenCalled();

      controller.abort();
      if (cancelled) reject(abortError);
      else resolve(response);
      await outcome;
      expect(dispose).toHaveBeenCalledTimes(1);

      processRequest.mockResolvedValueOnce(response);
      await expect(client.completeChat([{ role: "user", content: "Say hello again." }])).resolves.toEqual(completion);
      expect(dispose).toHaveBeenCalledTimes(2);
    },
  );
});
