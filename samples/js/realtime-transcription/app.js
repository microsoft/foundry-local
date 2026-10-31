import { createInterface } from "node:readline";

import { AudioSession, FoundryLocalManager, Item, ItemQueue, Request } from "foundry-local-sdk";

const modelAlias = process.argv[2] ?? "nemotron-speech-streaming-en-0.6b";
const sampleRate = 16_000;
const channels = 1;

const manager = FoundryLocalManager.create({
  appName: "foundry_local_samples",
  logLevel: "info",
});

let model;
let session;
let modelLoaded = false;
let audioInput;
let captureStarted = false;
let request;
let stream;
let transcription;
const queue = new ItemQueue();

try {
  await manager.downloadAndRegisterEps();
  model = await manager.catalog.getModel(modelAlias);
  if (!model.isCached) {
    console.log(`Downloading ${model.alias}...`);
    await model.download();
  }
  await model.load();
  modelLoaded = true;

  session = new AudioSession(model);
  request = new Request()
    .addItem(Item.audioDescriptor("pcm", sampleRate, channels))
    .addItem(queue)
    .setOptions({ additionalOptions: { language: "en" } });
  stream = session.processStreamingRequest(request);

  transcription = (async () => {
    for await (const item of stream) {
      if (item.type === "speechSegment") {
        const prefix = item.kind === "final" ? "\n[final] " : item.kind === "partial" ? "\r[partial] " : "";
        process.stdout.write(`${prefix}${item.text}`);
      }
    }
  })();
  transcription.catch(() => {});

  const { default: portAudio } = await import("naudiodon2");
  audioInput = portAudio.AudioIO({
    inOptions: {
      channelCount: channels,
      sampleFormat: portAudio.SampleFormat16Bit,
      sampleRate,
      framesPerBuffer: 1_600,
      maxQueue: 64,
    },
  });
  audioInput.on("data", (buffer) => {
    const chunk = new Uint8Array(buffer.byteLength);
    chunk.set(new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.byteLength));
    queue.push(Item.bytes(chunk));
  });
  audioInput.on("error", (error) => {
    console.error("\nMicrophone error:", error);
  });

  console.log("Listening. Speak into your microphone, then press Enter to stop.");
  audioInput.start();
  captureStarted = true;
  await new Promise((resolve) => {
    const readline = createInterface({ input: process.stdin, output: process.stdout });
    const stop = () => {
      process.removeListener("SIGINT", stop);
      readline.close();
      resolve();
    };
    readline.once("line", stop);
    readline.once("SIGINT", stop);
    readline.once("close", stop);
    process.once("SIGINT", stop);
  });

  await audioInput.quit();
  captureStarted = false;
  queue.markFinished();
  await transcription;
  const response = await stream.response;
  const result = response.output.find((item) => item.type === "speechResult");
  if (result?.type === "speechResult") {
    console.log(`\nTranscript: ${result.text}`);
  }
} finally {
  if (captureStarted) {
    await audioInput?.quit();
  }
  audioInput?.removeAllListeners();
  if (!queue.finished) {
    queue.markFinished();
  }
  request?.cancel();
  if (stream !== undefined && transcription !== undefined) {
    await Promise.allSettled([transcription, stream.response]);
  }
  queue.dispose();
  session?.dispose();
  try {
    if (model !== undefined && modelLoaded) {
      await model.unload();
    }
  } finally {
    manager.dispose();
  }
}
