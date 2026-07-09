// AudioSession tests.
//
// Mirrors embeddings-session.test.ts in structure. Coverage:
//   * JS-layer task validation (always runs against a cache-only catalog).
//   * Plumbing for audio inputs through Request / ItemQueue (no session
//     call needed, runs whenever the addon is built).
//   * Real-model non-streaming + streaming transcription against
//     whisper-tiny, gated by FOUNDRY_TEST_DATA_DIR (mirrors
//     sdk_v2/cpp/test/sdk_api/audio_transcriptions_test.cc).
//   * Real-model PCM streamed-input transcription against the nemotron
//     streaming-audio model, gated by FOUNDRY_TEST_DATA_DIR plus the
//     presence of testdata/Recording.pcm (mirrors
//     sdk_v2/cpp/test/sdk_api/streaming_audio_test.cc).
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { afterAll, afterEach, beforeAll, beforeEach, describe, expect, it } from "vitest";

import { ItemQueue } from "../src/item-queue.js";
import { type AudioItem, Item } from "../src/items.js";
import { FoundryLocalManager } from "../src/foundryLocalManager.js";
import { Request } from "../src/request.js";
import { AudioSession } from "../src/session.js";

import { haveNativePrereqs, nativePrereqsDiagnostic } from "./_fixtures/cacheOnlyManager.js";
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
const recordingPcmPath = resolve(testdataDir, "Recording.pcm");
const havePcmFixture = existsSync(recordingPcmPath);

// 100ms of audio at 16kHz mono s16le = 16000 samples/s * 0.1s * 2 bytes = 3200 bytes.
const CHUNK_BYTES = 3200;

// Key phrases drawn verbatim from the C++ audio transcription tests
// (sdk_v2/cpp/test/sdk_api/audio_transcriptions_test.cc and
// streaming_audio_test.cc). Models phrase the transcription slightly
// differently, so we check distinctive multi-word fragments rather than
// an exact match. Keep this list in sync with the C++ list.
const EXPECTED_PHRASES: ReadonlyArray<string> = [
  "give people",
  "more than one link",
  "live concert",
  "behind the scenes",
  "photo gallery",
  "album to purchase",
];

function extractText(item: Item): string {
  if (item.type === "speechResult") return item.text;
  if (item.type === "speechSegment") return item.text;
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

if (!haveNativePrereqs) {
  console.warn(`[AudioSession tests] SKIPPED — ${nativePrereqsDiagnostic}`);
}

// Local cache-only catalog with one ASR entry, one chat entry, and one
// entry whose `task` is omitted entirely. We do this inline rather than
// extending the shared cacheOnlyManager fixture so this slice has zero
// blast radius on the other test files.
const FIXTURE_CATALOG = {
  version: 1,
  savedAtUnix: 1713800000,
  models: [
    {
      id: "phi-4-mini-instruct-generic-cpu:2",
      name: "phi-4-mini-instruct-generic-cpu",
      version: 2,
      alias: "phi-4-mini-instruct",
      uri: "azureml://registries/azureml/models/phi-4-mini-instruct-generic-cpu/versions/2",
      providerType: "AzureFoundry",
      modelType: "ONNX",
      task: "chat-completion",
      publisher: "Microsoft",
      displayName: "Phi-4 Mini Instruct",
    },
    {
      id: "whisper-tiny-generic-cpu:1",
      name: "whisper-tiny-generic-cpu",
      version: 1,
      alias: "whisper-tiny",
      uri: "azureml://registries/azureml/models/whisper-tiny-generic-cpu/versions/1",
      providerType: "AzureFoundry",
      modelType: "ONNX",
      task: "automatic-speech-recognition",
      publisher: "OpenAI",
      displayName: "Whisper Tiny",
    },
    {
      id: "no-task-fixture-cpu:1",
      name: "no-task-fixture-cpu",
      version: 1,
      alias: "no-task-fixture",
      uri: "azureml://registries/azureml/models/no-task-fixture-cpu/versions/1",
      providerType: "AzureFoundry",
      modelType: "ONNX",
      // task intentionally omitted
      publisher: "Test",
      displayName: "No-task fixture",
    },
  ],
} as const;

interface AudioFixture {
  readonly manager: FoundryLocalManager;
  readonly tmpDir: string;
}

function setupAudioFixture(): AudioFixture {
  const tmpDir = mkdtempSync(join(tmpdir(), "fl-js-v2-audio-test-"));
  writeFileSync(join(tmpDir, "foundry.modelinfo.json"), JSON.stringify(FIXTURE_CATALOG, null, 2));
  const manager = FoundryLocalManager.create({
    appName: "foundry-local-js-sdk-v2-audio-tests",
    modelCacheDir: tmpDir,
    serviceEndpoint: "http://127.0.0.1:12345",
  });
  return { manager, tmpDir };
}

function teardownAudioFixture(fixture: AudioFixture): void {
  if (!fixture.manager.disposed) fixture.manager.dispose();
  rmSync(fixture.tmpDir, { recursive: true, force: true });
}

const describeIfBuilt = haveNativePrereqs ? describe : describe.skip;

describeIfBuilt("AudioSession constructor type guard", () => {
  it("throws TypeError when constructed with a non-Model argument", () => {
    expect(() => new AudioSession({} as never)).toThrow(TypeError);
    expect(() => new AudioSession({} as never)).toThrow(/Model/);
  });
});

describeIfBuilt("AudioSession task validation", () => {
  let fixture: AudioFixture | undefined;

  beforeAll(() => {
    fixture = setupAudioFixture();
  });

  afterAll(() => {
    if (fixture !== undefined) teardownAudioFixture(fixture);
  });

  it("rejects a wrong-task model with TypeError", async () => {
    if (fixture === undefined) throw new Error("fixture missing");
    const chatModel = await fixture.manager.catalog.getModel("phi-4-mini-instruct");
    expect(chatModel.info.task).toBe("chat-completion");
    expect(() => new AudioSession(chatModel)).toThrow(TypeError);
    expect(() => new AudioSession(chatModel)).toThrow(/automatic-speech-recognition/);
    expect(() => new AudioSession(chatModel)).toThrow(/chat-completion/);
  });

  it("rejects an unset-task model with TypeError reporting '(unset)'", async () => {
    if (fixture === undefined) throw new Error("fixture missing");
    const noTaskModel = await fixture.manager.catalog.getModel("no-task-fixture");
    expect(noTaskModel.info.task).toBeUndefined();
    expect(() => new AudioSession(noTaskModel)).toThrow(TypeError);
    expect(() => new AudioSession(noTaskModel)).toThrow(/automatic-speech-recognition/);
    expect(() => new AudioSession(noTaskModel)).toThrow(/got '\(unset\)'/);
  });

  // Construction with an ASR-task model succeeds at the JS validation
  // layer; it then fails inside the native ctor because `flSession_Create`
  // demands a loaded model. We assert the failure is NOT the TypeError
  // produced by JS validation — that proves JS validation passed and the
  // failure originated in the native layer (where it correctly belongs).
  it("passes JS validation for an ASR-task model and reaches the native ctor", async () => {
    if (fixture === undefined) throw new Error("fixture missing");
    const asrModel = await fixture.manager.catalog.getModel("whisper-tiny");
    expect(asrModel.info.task).toBe("automatic-speech-recognition");
    let err: unknown;
    try {
      new AudioSession(asrModel);
    } catch (e) {
      err = e;
    }
    expect(err).toBeDefined();
    expect(err).not.toBeInstanceOf(TypeError);
  });
});

describeIfBuilt("AudioSession audio-input plumbing", () => {
  it("Request.addItem accepts an ItemQueue of bytes chunks for live PCM input", () => {
    using q = new ItemQueue();
    q.push(Item.bytes(new Uint8Array([0x01, 0x02, 0x03, 0x04])));
    q.push(Item.bytes(new Uint8Array([0x05, 0x06, 0x07, 0x08])));
    q.markFinished();
    expect(q.size).toBe(2);
    expect(q.finished).toBe(true);

    const req = new Request();
    req.addItem(q);
    expect(req.itemCount).toBe(1);
  });

  it("Item.audioFromData round-trips through Request (zero-copy bytes path)", () => {
    const data = new Uint8Array([10, 20, 30, 40, 50]);
    const req = new Request();
    req.addItem(Item.audioFromData("wav", data));
    expect(req.itemCount).toBe(1);
    const round = req.getItem(0) as AudioItem;
    expect(round.type).toBe("audio");
    expect(round.format).toBe("wav");
    expect(round.data).toBeDefined();
    expect(round.data && Array.from(round.data)).toEqual([10, 20, 30, 40, 50]);
  });
});

// Real-model non-streaming transcription against whisper-tiny. Mirrors
// AudioSessionFixture.TranscribeFromUri / TranscribeWithRequestLanguageOption
// in sdk_v2/cpp/test/sdk_api/audio_transcriptions_test.cc. Skipped (NOT
// passed) when FOUNDRY_TEST_DATA_DIR is unset — surfaced via
// `describe.skipIf` so vitest reports it in the suite's `skipped` count.
describe.skipIf(!haveTestModelCache)("AudioSession (real whisper-tiny model)", () => {
  let fixture: RealModelManagerFixture | undefined;
  let session: AudioSession | undefined;

  beforeAll(async () => {
    fixture = await setupRealModelManager({
      namePreference: "openai-whisper-tiny-generic-cpu",
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

  it(
    "transcribes Recording.mp3 from URI and contains the expected key phrases",
    async () => {
      if (session === undefined) throw new Error("session missing");
      const req = new Request()
        .addItem(Item.audioFromUri(recordingMp3Path))
        .setOptions({ search: { temperature: 0 }, additionalOptions: { language: "en" } });

      const resp = await session.processRequest(req);

      expect(resp.output.length).toBeGreaterThanOrEqual(1);
      expect(["stop", "length", "toolCalls", "error", "none"]).toContain(resp.finishReason);

      const result = resp.output.find((it) => it.type === "speechResult");
      expect(result, "expected a speechResult item in the aggregated Response").toBeDefined();
      if (result?.type !== "speechResult") throw new Error("unreachable");
      expect(result.segments.length).toBeGreaterThan(0);

      const text = result.text;
      expect(text.length).toBeGreaterThan(0);
      const lower = text.toLowerCase();
      for (const phrase of EXPECTED_PHRASES) {
        expect(lower, `Expected transcription to contain '${phrase}'. Got: ${text}`).toContain(phrase);
      }

      expect(resp.usage.promptTokens).toBeGreaterThan(0);
      expect(resp.usage.completionTokens).toBeGreaterThan(0);
      expect(resp.usage.totalTokens).toBe(resp.usage.promptTokens + resp.usage.completionTokens);
    },
    2 * 60_000,
  );

  // Mirrors AudioSessionTests.Transcribe_Streaming_Succeeds in
  // sdk_v2/cs/test/FoundryLocal.Tests/AudioSessionTests.cs — iterates
  // streamed SpeechSegment items, accumulates text, and asserts that the
  // callback fired and the joined transcript contains the expected phrases.
  it(
    "processStreamingRequest yields SpeechSegment items for whisper-tiny",
    async () => {
      if (session === undefined) throw new Error("session missing");
      const req = new Request()
        .addItem(Item.audioFromUri(recordingMp3Path))
        .setOptions({ search: { temperature: 0 }, additionalOptions: { language: "en" } });

      let segmentCount = 0;
      let acc = "";
      for await (const item of session.processStreamingRequest(req)) {
        if (item.type === "speechSegment") {
          segmentCount++;
          acc += item.text;
        }
      }

      expect(segmentCount).toBeGreaterThan(0);
      expect(acc.length).toBeGreaterThan(0);
      const lower = acc.toLowerCase();
      for (const phrase of EXPECTED_PHRASES) {
        expect(lower, `Expected streamed transcription to contain '${phrase}'. Got: ${acc}`).toContain(phrase);
      }
    },
    2 * 60_000,
  );

  // Mirrors AudioSessionTests.Transcribe_Streaming_FinalResponse_AggregatesTranscript.
  // Drains the streamed segments, then awaits `.response` and verifies the
  // terminal Response carries a SpeechResultItem aggregating the transcript.
  it(
    "processStreamingRequest exposes the aggregated SpeechResult via .response",
    async () => {
      if (session === undefined) throw new Error("session missing");
      const req = new Request()
        .addItem(Item.audioFromUri(recordingMp3Path))
        .setOptions({ search: { temperature: 0 }, additionalOptions: { language: "en" } });

      const stream = session.processStreamingRequest(req);

      let streamedCount = 0;
      let streamedText = "";
      for await (const item of stream) {
        if (item.type === "speechSegment") {
          streamedCount++;
          streamedText += item.text;
        }
      }

      expect(streamedCount).toBeGreaterThan(0);
      expect(streamedText.length).toBeGreaterThan(0);

      const final = await stream.response;
      expect(final.output.length).toBeGreaterThanOrEqual(1);
      const result = final.output.find((it) => it.type === "speechResult");
      expect(result, "expected a speechResult item in the terminal Response").toBeDefined();
      if (result?.type !== "speechResult") throw new Error("unreachable");

      expect(result.text.length).toBeGreaterThan(0);
      const lower = result.text.toLowerCase();
      for (const phrase of EXPECTED_PHRASES) {
        expect(lower, `Expected aggregated transcription to contain '${phrase}'. Got: ${result.text}`).toContain(phrase);
      }
    },
    2 * 60_000,
  );
});

// Streaming PCM transcription against a real nemotron streaming-audio
// model. Mirrors sdk_v2/cpp/test/sdk_api/streaming_audio_test.cc
// (StreamRecordingInChunksAndValidateTranscription).
//
// Gated by:
//   * FOUNDRY_TEST_DATA_DIR (via `describe.skipIf(!haveTestModelCache)`)
//   * presence of sdk_v2/testdata/Recording.pcm (per-test `it.skipIf`)
// Both gating paths surface as vitest "skipped", not "passed".
describe.skipIf(!haveTestModelCache)("AudioSession (real nemotron streaming model, PCM)", () => {
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

  // Streamed input + streamed output. Mirrors the C++
  // StreamingAudioFixture.StreamingCallbackReceivesTokens test in
  // sdk_v2/cpp/test/sdk_api/streaming_audio_test.cc: pushes PCM chunks
  // into the ItemQueue while concurrently iterating speech segments
  // emitted by the model via processStreamingRequest.
  it.skipIf(!havePcmFixture)(
    "streams PCM via ItemQueue and consumes SpeechSegment items via processStreamingRequest",
    async () => {
      if (session === undefined) throw new Error("session missing");

      const pcm = readFileSync(recordingPcmPath);
      expect(pcm.length).toBeGreaterThan(0);
      const chunks = splitIntoChunks(pcm, CHUNK_BYTES);
      expect(chunks.length).toBeGreaterThan(1);

      const audio = Item.audioDescriptor("pcm", 16000, 1);

      using queue = new ItemQueue();

      const req = new Request();
      req.addItem(audio);
      req.addItem(queue);

      // Start the stream eagerly. The native ProcessRequest runs on a
      // libuv worker, so pushing chunks from this same JS turn just
      // queues them — the native consumer drains them while we wait on
      // the async iterator below.
      const stream = session.processStreamingRequest(req);

      for (const chunk of chunks) {
        const copy = new Uint8Array(chunk.length);
        copy.set(chunk);
        queue.push(Item.bytes(copy));
      }
      queue.markFinished();

      let segmentCount = 0;
      let streamedText = "";
      for await (const item of stream) {
        // Audio sessions only emit speechSegment items via the streaming
        // callback. Asserting the type here mirrors the C++ EXPECT_EQ on
        // FOUNDRY_LOCAL_ITEM_SPEECH_SEGMENT.
        expect(item.type).toBe("speechSegment");
        if (item.type === "speechSegment") {
          segmentCount++;
          streamedText += item.text;
        }
      }

      expect(segmentCount).toBeGreaterThan(0);
      expect(streamedText.length).toBeGreaterThan(0);

      const lower = streamedText.toLowerCase();
      for (const phrase of EXPECTED_PHRASES) {
        expect(lower, `Expected streamed transcription to contain '${phrase}'. Got: ${streamedText}`).toContain(phrase);
      }

      // The terminal Response carries an aggregated SpeechResultItem
      // built from the streamed segments. Validate it matches what we
      // accumulated from the per-segment callbacks.
      const final = await stream.response;
      expect(["stop", "length", "toolCalls", "error", "none"]).toContain(final.finishReason);
      const result = final.output.find((it) => it.type === "speechResult");
      expect(result, "expected a speechResult item in the terminal Response").toBeDefined();
      if (result?.type !== "speechResult") throw new Error("unreachable");
      expect(result.segments.length).toBe(segmentCount);
      expect(result.text).toBe(streamedText);
    },
    5 * 60_000,
  );
});
