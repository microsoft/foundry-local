// OpenAI-pass-through clients scope sessions to individual requests; dispose() only prevents future calls.
// Use the chat fixture for native session-release coverage and each client's integration suite for its modality.
import { afterAll, beforeAll, describe, expect, it } from "vitest";

import { AudioClient } from "../src/openai/audioClient.js";
import { ChatClient } from "../src/openai/chatClient.js";
import { EmbeddingClient } from "../src/openai/embeddingClient.js";

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

describe.skipIf(!haveTestModelCache)("OpenAI client dispose()", () => {
  let fixture: RealModelManagerFixture | undefined;

  beforeAll(async () => {
    fixture = await setupRealModelManager();
  }, 5 * 60_000);

  afterAll(() => {
    teardownRealModelManager(fixture);
  });

  describe("ChatClient", () => {
    it("disposed is false on a fresh client", () => {
      if (fixture === undefined) throw new Error("fixture missing");
      const client = fixture.model.createChatClient();
      try {
        expect(client.disposed).toBe(false);
      } finally {
        client.dispose();
      }
    });

    it("dispose() of a client that never created a session is a no-op (does not throw)", () => {
      if (fixture === undefined) throw new Error("fixture missing");
      const client = fixture.model.createChatClient();
      expect(() => client.dispose()).not.toThrow();
      expect(client.disposed).toBe(true);
    });

    it("dispose() is idempotent — calling twice does not throw", () => {
      if (fixture === undefined) throw new Error("fixture missing");
      const client = fixture.model.createChatClient();
      client.dispose();
      expect(() => client.dispose()).not.toThrow();
      expect(client.disposed).toBe(true);
    });

    it("completeChat after dispose() throws a clear error", async () => {
      if (fixture === undefined) throw new Error("fixture missing");
      const client = fixture.model.createChatClient();
      client.dispose();
      await expect(client.completeChat([{ role: "user", content: "hi" }])).rejects.toThrow(/already disposed/);
    });

    it("completeStreamingChat after dispose() throws synchronously", () => {
      if (fixture === undefined) throw new Error("fixture missing");
      const client = fixture.model.createChatClient();
      client.dispose();
      // The disposed guard runs before returning an iterator; session creation itself is deferred until iteration.
      expect(() => client.completeStreamingChat([{ role: "user", content: "hi" }])).toThrow(/already disposed/);
    });

    it(
      "completed inference releases its session without requiring client disposal",
      async () => {
        if (fixture === undefined) throw new Error("fixture missing");
        const model = fixture.model;

        if (!(await model.isLoaded())) {
          await model.load();
        }
        using client = model.createChatClient();
        client.settings.maxTokens = 16;
        client.settings.temperature = 0;
        const result = await client.completeChat([{ role: "user", content: "Say 'ok' and nothing else." }]);
        expect(result).toBeDefined();

        try {
          expect(client.disposed).toBe(false);
          await expect(model.unload()).resolves.toBeUndefined();
          expect(await model.isLoaded()).toBe(false);
        } finally {
          await model.load();
        }
      },
      3 * 60_000,
    );

    it("[Symbol.dispose] disposes the client (using-statement parity)", () => {
      if (fixture === undefined) throw new Error("fixture missing");
      const client = fixture.model.createChatClient();
      expect(typeof client[Symbol.dispose]).toBe("function");
      client[Symbol.dispose]();
      expect(client.disposed).toBe(true);
      // Second call still safe.
      expect(() => client[Symbol.dispose]()).not.toThrow();
    });
  });

  describe("AudioClient (no-session dispose paths)", () => {
    it("dispose() of a never-used AudioClient is a no-op", () => {
      if (fixture === undefined) throw new Error("fixture missing");
      const client = new AudioClient(fixture.model);
      expect(client.disposed).toBe(false);
      expect(() => client.dispose()).not.toThrow();
      expect(client.disposed).toBe(true);
      expect(() => client.dispose()).not.toThrow();
    });

    it("transcribe after dispose() throws", async () => {
      if (fixture === undefined) throw new Error("fixture missing");
      const client = new AudioClient(fixture.model);
      client.dispose();
      await expect(client.transcribe("anything.wav")).rejects.toThrow(/already disposed/);
    });
  });

  describe("EmbeddingClient (no-session dispose paths)", () => {
    it("dispose() of a never-used EmbeddingClient is a no-op", () => {
      if (fixture === undefined) throw new Error("fixture missing");
      const client = new EmbeddingClient(fixture.model);
      expect(client.disposed).toBe(false);
      expect(() => client.dispose()).not.toThrow();
      expect(client.disposed).toBe(true);
      expect(() => client.dispose()).not.toThrow();
    });

    it("generateEmbedding after dispose() throws", async () => {
      if (fixture === undefined) throw new Error("fixture missing");
      const client = new EmbeddingClient(fixture.model);
      client.dispose();
      await expect(client.generateEmbedding("hello")).rejects.toThrow(/already disposed/);
    });
  });
});
