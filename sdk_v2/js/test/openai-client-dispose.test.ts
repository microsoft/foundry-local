// Dispose semantics for the OpenAI-pass-through clients (`ChatClient`,
// `AudioClient`, `EmbeddingClient`). Each client lazily constructs an inner
// `Session` on first use; without an explicit `dispose()` the inner session
// pins the loaded model and `model.unload()` rejects with
// FoundryLocalError / code 4 ("session(s) still using it").
//
// The deep coverage runs against the chat fixture because it loads the
// smallest model in the catalog. AudioClient and EmbeddingClient share the
// exact same dispose machinery as ChatClient — we exercise their no-session
// paths against the chat fixture too (a client that never inferred has no
// session-type dependency) and rely on their respective integration test
// files for the with-session paths.
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
      await expect(
        client.completeChat([{ role: "user", content: "hi" }]),
      ).rejects.toThrow(/already disposed/);
    });

    it("completeStreamingChat after dispose() throws synchronously", () => {
      if (fixture === undefined) throw new Error("fixture missing");
      const client = fixture.model.createChatClient();
      client.dispose();
      // completeStreamingChat resolves the session up-front so the throw
      // surfaces synchronously at the call site (before returning an iterator).
      expect(() => client.completeStreamingChat([{ role: "user", content: "hi" }])).toThrow(
        /already disposed/,
      );
    });

    it(
      "after running inference, dispose() releases the inner session so model.unload() succeeds",
      async () => {
        if (fixture === undefined) throw new Error("fixture missing");
        const model = fixture.model;

        // Ensure the model is loaded for the run, then exercise the client
        // so the lazy inner ChatSession actually exists.
        if (!(await model.isLoaded())) {
          await model.load();
        }
        const client = model.createChatClient();
        client.settings.maxTokens = 16;
        client.settings.temperature = 0;
        const result = await client.completeChat([
          { role: "user", content: "Say 'ok' and nothing else." },
        ]);
        expect(result).toBeDefined();

        // Without dispose() this unload() would reject with FoundryLocalError code 4
        // ("N session(s) still using it"). dispose() releases the inner ChatSession
        // so unload() must succeed.
        client.dispose();
        await expect(model.unload()).resolves.toBeUndefined();
        expect(await model.isLoaded()).toBe(false);

        // Restore the loaded state for any later test in the suite.
        await model.load();
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
