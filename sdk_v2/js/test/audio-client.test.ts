// AudioClient (V1 OpenAI-JSON pass-through) against a real loaded
// whisper-tiny model. Gated by FOUNDRY_TEST_DATA_DIR. Mirrors
// audio-session.test.ts non-streaming coverage.
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { afterAll, beforeAll, describe, expect, it } from "vitest";

import type { AudioClient } from "../src/openai/audioClient.js";

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
const recordingMp3Path = resolve(testdataDir, "Recording.mp3");

// Key phrases mirrored from audio-session.test.ts EXPECTED_PHRASES.
const EXPECTED_PHRASES: ReadonlyArray<string> = [
  "give people",
  "more than one link",
  "live concert",
  "behind the scenes",
  "photo gallery",
  "album to purchase",
];

// biome-ignore lint/suspicious/noExplicitAny: OpenAI response objects are user-shaped JSON.
type Json = any;

function transcriptionText(result: Json): string {
  // OpenAI audio.transcriptions returns `{ text: string }`. The pass-through
  // may also surface alternative shapes; accept either.
  if (typeof result?.text === "string") return result.text;
  if (typeof result?.transcription === "string") return result.transcription;
  return "";
}

describe.skipIf(!haveTestModelCache)("AudioClient (real whisper-tiny model, V1 OpenAI-JSON pass-through)", () => {
  let fixture: RealModelManagerFixture | undefined;
  let client: AudioClient | undefined;

  beforeAll(async () => {
    fixture = await setupRealModelManager({
      namePreference: "openai-whisper-tiny-generic-cpu",
      task: "automatic-speech-recognition",
    });
    if (fixture !== undefined) {
      client = fixture.model.createAudioClient();
      client.settings.language = "en";
      client.settings.temperature = 0;
    }
  }, 5 * 60_000);

  afterAll(() => {
    client?.dispose();
    client = undefined;
    teardownRealModelManager(fixture);
  });

  it(
    "transcribe returns an OpenAI-shaped result with the expected key phrases",
    async () => {
      if (client === undefined) throw new Error("client missing");
      const result = await client.transcribe(recordingMp3Path);
      const text = transcriptionText(result);
      expect(text.length).toBeGreaterThan(0);
      const lower = text.toLowerCase();
      for (const phrase of EXPECTED_PHRASES) {
        expect(lower, `Expected transcription to contain '${phrase}'. Got: ${text}`).toContain(phrase);
      }
    },
    2 * 60_000,
  );

  it("rejects an empty filePath with the documented message", async () => {
    if (client === undefined) throw new Error("client missing");
    await expect(client.transcribe("")).rejects.toThrow("filePath must be a non-empty string.");
  });
});
