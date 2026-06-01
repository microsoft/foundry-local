// LiveAudioTranscriptionSession (V1 surface) against a real streaming-audio
// model, mirroring audio-session-streaming.test.ts but exercising the V1
// AudioClient.createLiveTranscriptionSession() factory.
import { existsSync, readFileSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { afterAll, afterEach, beforeAll, beforeEach, describe, expect, it } from "vitest";

import type { AudioClient } from "../src/openai/audioClient.js";
import type { LiveAudioTranscriptionSession } from "../src/openai/liveAudioSession.js";

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

const hereDir = fileURLToPath(new URL(".", import.meta.url));
const testdataDir = resolve(hereDir, "..", "..", "testdata");
const recordingPcmPath = resolve(testdataDir, "Recording.pcm");
const havePcmFixture = existsSync(recordingPcmPath);

const EXPECTED_PHRASES: ReadonlyArray<string> = [
  "give people",
  "more than one link",
  "live concert",
  "behind the scenes",
  "photo gallery",
  "album to purchase",
];

// 100ms of audio at 16kHz mono s16le = 3200 bytes.
const CHUNK_BYTES = 3200;

function splitIntoChunks(data: Uint8Array, chunkSize: number): Uint8Array[] {
  const out: Uint8Array[] = [];
  for (let off = 0; off < data.length; off += chunkSize) {
    out.push(data.subarray(off, Math.min(off + chunkSize, data.length)));
  }
  return out;
}

describe.skipIf(!haveTestModelCache)("LiveAudioTranscriptionSession (V1, real nemotron streaming model)", () => {
  let fixture: RealModelManagerFixture | undefined;
  let client: AudioClient | undefined;
  let session: LiveAudioTranscriptionSession | undefined;

  beforeAll(async () => {
    fixture = await setupRealModelManager({
      namePreference: "nemotron-speech-streaming-en-0.6b-generic-cpu",
      task: "automatic-speech-recognition",
    });
    if (fixture !== undefined) {
      client = fixture.model.createAudioClient();
    }
  }, 5 * 60_000);

  afterAll(() => {
    client?.dispose();
    client = undefined;
    teardownRealModelManager(fixture);
  });

  beforeEach(() => {
    if (client === undefined) throw new Error("client missing");
    session = client.createLiveTranscriptionSession();
  });

  afterEach(async () => {
    if (session !== undefined) {
      await session.dispose();
    }
    session = undefined;
  });

  it("dispose is awaitable before start", async () => {
    if (session === undefined) throw new Error("session missing");

    await expect(session.dispose()).resolves.toBeUndefined();
  });

  it(
    "can be started again after stop",
    async () => {
      if (session === undefined) throw new Error("session missing");

      await session.start();
      await session.stop();

      await expect(session.start()).resolves.toBeUndefined();
      await expect(session.stop()).resolves.toBeUndefined();
    },
    60_000,
  );

  it.skipIf(!havePcmFixture)(
    "streams Recording.pcm in 100ms chunks via append() and aggregates getStream() into the expected transcript",
    async () => {
      if (session === undefined) throw new Error("session missing");

      const pcm = readFileSync(recordingPcmPath);
      expect(pcm.length).toBeGreaterThan(0);
      const chunks = splitIntoChunks(pcm, CHUNK_BYTES);
      expect(chunks.length).toBeGreaterThan(1);

      await session.start();

      // Push the chunks and signal end-of-input in the background while a
      // concurrent consumer drains the response stream.
      const producer = (async () => {
        for (const chunk of chunks) {
          const copy = new Uint8Array(chunk.length);
          copy.set(chunk);
          await (session as LiveAudioTranscriptionSession).append(copy);
        }
        await (session as LiveAudioTranscriptionSession).stop();
      })();

      let partialText = "";
      let finalText = "";
      let finalCount = 0;
      for await (const resp of session.getStream()) {
        let pieceText = "";
        for (const part of resp.content) {
          if (typeof part.text === "string") pieceText += part.text;
          else if (typeof part.transcript === "string") pieceText += part.transcript;
        }
        if (resp.is_final) {
          finalText += pieceText;
          finalCount++;
        } else {
          partialText += pieceText;
        }
      }
      await producer;

      // Partial (per-token, is_final=false) stream must cover the transcript.
      expect(partialText.length).toBeGreaterThan(0);
      const lowerPartial = partialText.toLowerCase();
      for (const phrase of EXPECTED_PHRASES) {
        expect(lowerPartial, `Expected partial transcription to contain '${phrase}'. Got: ${partialText}`).toContain(
          phrase,
        );
      }

      // Exactly one final aggregate (is_final=true) sourced from the
      // terminal Response's text item — must also cover the transcript.
      expect(finalCount).toBe(1);
      expect(finalText.length).toBeGreaterThan(0);
      const lowerFinal = finalText.toLowerCase();
      for (const phrase of EXPECTED_PHRASES) {
        expect(lowerFinal, `Expected final aggregate to contain '${phrase}'. Got: ${finalText}`).toContain(phrase);
      }
    },
    5 * 60_000,
  );
});
