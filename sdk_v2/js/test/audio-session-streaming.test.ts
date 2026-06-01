// Streaming PCM transcription against a real nemotron streaming-audio
// model. Mirrors sdk_v2/cpp/test/sdk_api/streaming_audio_test.cc
// (StreamRecordingInChunksAndValidateTranscription).
//
// Gated by:
//   * FOUNDRY_TEST_DATA_DIR (via `describe.skipIf(!haveTestModelCache)`)
//   * presence of sdk_v2/testdata/Recording.pcm (per-test `it.skipIf`)
// Both gating paths surface as vitest "skipped", not "passed".
import { existsSync, readFileSync } from "node:fs";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { afterAll, afterEach, beforeAll, beforeEach, describe, expect, it } from "vitest";

import { ItemQueue } from "../src/item-queue.js";
import { Item } from "../src/items.js";
import { Request } from "../src/request.js";
import { AudioSession } from "../src/session.js";

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

// Key phrases drawn verbatim from streaming_audio_test.cc. Keep in sync
// with the list in audio-session.test.ts.
const EXPECTED_PHRASES: ReadonlyArray<string> = [
  "give people",
  "more than one link",
  "live concert",
  "behind the scenes",
  "photo gallery",
  "album to purchase",
];

// 100ms of audio at 16kHz mono s16le = 16000 samples/s * 0.1s * 2 bytes = 3200 bytes.
const CHUNK_BYTES = 3200;

function extractText(item: Item): string {
  if (item.type === "text") return item.text;
  if (item.type === "message") {
    if (typeof item.content === "string") return item.content;
    if (item.parts) {
      let acc = "";
      for (const p of item.parts) {
        if (p.type === "text") acc += p.text;
      }
      return acc;
    }
  }
  return "";
}

function collectResponseText(output: ReadonlyArray<Item>): string {
  return output.map(extractText).join("");
}

function splitIntoChunks(data: Uint8Array, chunkSize: number): Uint8Array[] {
  const out: Uint8Array[] = [];
  for (let off = 0; off < data.length; off += chunkSize) {
    out.push(data.subarray(off, Math.min(off + chunkSize, data.length)));
  }
  return out;
}

describe.skipIf(!haveTestModelCache)("AudioSession streaming (nemotron PCM)", () => {
  let fixture: RealModelManagerFixture | undefined;
  let session: AudioSession | undefined;

  beforeAll(async () => {
    fixture = await setupRealModelManager({
      namePreference: "nemotron-speech-streaming-en-0.6b-generic-cpu",
      task: "automatic-speech-recognition",
    });
  }, 5 * 60_000);

  afterAll(() => {
    teardownRealModelManager(fixture);
  });

  beforeEach(() => {
    if (fixture === undefined) throw new Error("fixture missing");
    session = new AudioSession(fixture.model);
  });

  afterEach(() => {
    session?.dispose();
    session = undefined;
  });

  // Per-test gate on the PCM fixture file so an absent testdata file
  // surfaces as a skip rather than a hard failure.
  it.skipIf(!havePcmFixture)(
    "streams Recording.pcm in 100ms chunks via ItemQueue and validates transcription",
    async () => {
      if (session === undefined) throw new Error("session missing");

      const pcm = readFileSync(recordingPcmPath);
      expect(pcm.length).toBeGreaterThan(0);
      const chunks = splitIntoChunks(pcm, CHUNK_BYTES);
      expect(chunks.length).toBeGreaterThan(1);

      // Audio format descriptor — no initial data, declares sample rate /
      // channels so the model knows how to interpret the streamed bytes
      // supplied via the accompanying ItemQueue. Mirrors the C++
      // `AudioFromData("pcm", nullptr, 0, 16000, 1)` shape.
      const audio = Item.audioDescriptor("pcm", 16000, 1);

      using queue = new ItemQueue();

      const req = new Request();
      req.addItem(audio);
      req.addItem(queue);

      // Kick off the non-streaming session call without awaiting. The
      // native ProcessRequest runs on a libuv worker thread, so the JS
      // thread is free to push chunks into the queue concurrently —
      // analogous to the C++ test's `std::async(std::launch::async, ...)`.
      const pending = session.processRequest(req);

      for (const chunk of chunks) {
        // Copy each chunk into its own Uint8Array so the pinned buffer
        // handed to the native layer is decoupled from the underlying
        // file-backed buffer slice.
        const copy = new Uint8Array(chunk.length);
        copy.set(chunk);
        queue.push(Item.bytes(copy));
      }
      queue.markFinished();

      const resp = await pending;

      expect(["stop", "length", "toolCalls", "error", "none"]).toContain(resp.finishReason);
      expect(resp.output.length).toBeGreaterThanOrEqual(1);

      const text = collectResponseText(resp.output);
      expect(text.length).toBeGreaterThan(0);
      const lower = text.toLowerCase();
      for (const phrase of EXPECTED_PHRASES) {
        expect(lower, `Expected transcription to contain '${phrase}'. Got: ${text}`).toContain(phrase);
      }
    },
    5 * 60_000,
  );
});
