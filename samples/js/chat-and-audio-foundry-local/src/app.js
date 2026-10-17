// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

import { AudioSession, ChatSession, FoundryLocalManager, Item, Request } from "foundry-local-sdk";
import path from "path";

const CHAT_MODEL = "phi-3.5-mini";
const WHISPER_MODEL = "whisper-tiny";

function collectText(items) {
  const parts = [];
  for (const item of items) {
    if (item.type === "text" && (item.textType ?? "default") === "default") {
      parts.push(item.text);
    } else if (item.type === "message") {
      if (Array.isArray(item.parts)) {
        for (const part of item.parts) {
          if (part.type === "text" && (part.textType ?? "default") === "default") {
            parts.push(part.text);
          }
        }
      } else if (typeof item.content === "string") {
        parts.push(item.content);
      }
    }
  }
  return parts.join("");
}

function requireSpeechResult(response) {
  const result = response.output.find((item) => item.type === "speechResult");
  if (!result) {
    throw new Error("Expected a speechResult item in the audio response.");
  }
  return result;
}

async function main() {
  console.log("Initializing Foundry Local SDK...");
  const manager = FoundryLocalManager.create({
    appName: "foundry_local_samples",
    logLevel: "info",
  });

  let currentEp = "";
  await manager.downloadAndRegisterEps((epName, percent) => {
    if (epName !== currentEp) {
      if (currentEp !== "") process.stdout.write("\n");
      currentEp = epName;
    }
    process.stdout.write(`\r  ${epName.padEnd(30)}  ${percent.toFixed(1).padStart(5)}%`);
  });
  if (currentEp !== "") process.stdout.write("\n");

  const catalog = manager.catalog;

  console.log("\n--- Loading models ---");
  const chatModel = await catalog.getModel(CHAT_MODEL);
  const whisperModel = await catalog.getModel(WHISPER_MODEL);

  if (!chatModel) {
    throw new Error(`Chat model "${CHAT_MODEL}" not found. Run "foundry model list" to see available models.`);
  }

  if (!whisperModel) {
    throw new Error(`Whisper model "${WHISPER_MODEL}" not found. Run "foundry model list" to see available models.`);
  }

  if (!chatModel.isCached) {
    console.log(`Downloading ${CHAT_MODEL}...`);
    await chatModel.download((progress) => {
      process.stdout.write(`\r  ${CHAT_MODEL}: ${progress.toFixed(1)}%`);
    });
    console.log();
  }

  if (!whisperModel.isCached) {
    console.log(`Downloading ${WHISPER_MODEL}...`);
    await whisperModel.download((progress) => {
      process.stdout.write(`\r  ${WHISPER_MODEL}: ${progress.toFixed(1)}%`);
    });
    console.log();
  }

  console.log(`Loading ${CHAT_MODEL}...`);
  await chatModel.load();
  console.log(`Loading ${WHISPER_MODEL}...`);
  await whisperModel.load();
  console.log("Both models loaded.\n");

  const audioSession = new AudioSession(whisperModel);
  const chatSession = new ChatSession(chatModel);
  chatSession.setOptions({
    search: {
      temperature: 0.7,
      maxOutputTokens: 500,
    },
  });

  try {
    console.log("=== Step 1: Audio Transcription ===");
    const audioFilePath = path.resolve("recording.mp3");
    const transcriptionResponse = await audioSession.processRequest(
      new Request()
        .addItem(Item.audioFromUri(audioFilePath))
        .setOptions({ additionalOptions: { language: "en" } }),
    );
    const transcription = requireSpeechResult(transcriptionResponse).text;
    console.log("You said:", transcription);

    console.log("\n=== Step 2: AI Analysis ===");
    console.log("Generating summary...\n");

    const stream = chatSession.processStreamingRequest(
      new Request()
        .addItem(
          Item.systemMessage(
            "You are a helpful assistant. Summarize the following transcribed audio and extract key themes and action items.",
          ),
        )
        .addItem(Item.userMessage(transcription)),
    );

    for await (const item of stream) {
      const text = collectText([item]);
      if (text) {
        process.stdout.write(text);
      }
    }
    console.log("\n");
  } finally {
    chatSession.dispose();
    audioSession.dispose();
    await chatModel.unload();
    await whisperModel.unload();
    console.log("Done.");
  }
}

main().catch(console.error);
