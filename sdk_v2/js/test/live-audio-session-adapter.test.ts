import { afterEach, describe, expect, it, vi } from "vitest";

import type { Model } from "../src/model.js";
import { LiveAudioTranscriptionSession } from "../src/openai/liveAudioSession.js";
import { Request } from "../src/request.js";

vi.mock("../src/item-queue.js", () => ({
  ItemQueue: class {
    push(): void {}
    markFinished(): void {}
    dispose(): void {}
  },
}));

vi.mock("../src/items.js", () => ({
  Item: {
    audioDescriptor: () => ({ type: "audio" }),
  },
}));

vi.mock("../src/request.js", () => ({
  Request: class {
    addItem(): this {
      return this;
    }

    setOptions(): this {
      return this;
    }
  },
}));

vi.mock("../src/session.js", () => ({
  AudioSession: class {
    processStreamingRequest() {
      return {
        async *[Symbol.asyncIterator](): AsyncGenerator<never> {},
        response: Promise.resolve({ output: [] }),
      };
    }

    dispose(): void {}
  },
}));

const model = { id: "adapter-test-model" } as Model;

describe("LiveAudioTranscriptionSession request adapter", () => {
  afterEach(() => {
    vi.restoreAllMocks();
  });

  it("forwards the snapshotted language under the lowercase request option", async () => {
    const setOptions = vi.spyOn(Request.prototype, "setOptions");
    const session = new LiveAudioTranscriptionSession(model);
    session.settings.language = "fr";

    try {
      await session.start();

      expect(setOptions).toHaveBeenCalledOnce();
      expect(setOptions).toHaveBeenCalledWith({ additionalOptions: { language: "fr" } });
    } finally {
      await session.dispose();
    }
  });

  it("does not set request options when language is unset", async () => {
    const setOptions = vi.spyOn(Request.prototype, "setOptions");
    const session = new LiveAudioTranscriptionSession(model);

    try {
      await session.start();

      expect(setOptions).not.toHaveBeenCalled();
    } finally {
      await session.dispose();
    }
  });
});
