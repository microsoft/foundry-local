// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

import { ChatSession, FoundryLocalManager, Item, Request } from "foundry-local-sdk";

const preferredModelNames = ["qwen2.5-0.5b", "qwen3.5-0.8b"];
const modelCacheDir = process.env.FOUNDRY_LOCAL_SAMPLE_CACHE_DIR;
const managerConfig = { appName: "FoundryLocalBasicChat" };

if (modelCacheDir?.trim()) {
  managerConfig.modelCacheDir = modelCacheDir;
}

function isCpuChatModel(model) {
  const info = model.info;
  const executionProvider = info.runtime?.executionProvider ?? info.executionProvider;

  return (
    ["chat-completion", "vision-language-chat"].includes(info.task?.toLowerCase()) &&
    (info.deviceType === "CPU" ||
      info.runtime?.deviceType === "CPU" ||
      executionProvider?.toLowerCase() === "cpuexecutionprovider")
  );
}

function isPreferredModel(model) {
  const names = [model.alias, model.info.name, model.id];
  return names.some((name) => preferredModelNames.some((preferred) => name.toLowerCase().startsWith(preferred)));
}

function compareModels(left, right) {
  const preferenceDifference = Number(isPreferredModel(right)) - Number(isPreferredModel(left));
  if (preferenceDifference !== 0) {
    return preferenceDifference;
  }

  const leftSize = left.info.fileSizeMb ?? Number.POSITIVE_INFINITY;
  const rightSize = right.info.fileSizeMb ?? Number.POSITIVE_INFINITY;
  if (leftSize !== rightSize) {
    return leftSize - rightSize;
  }

  return left.id < right.id ? -1 : left.id > right.id ? 1 : 0;
}

function extractText(item) {
  if (item.type === "text") {
    return item.text;
  }
  if (item.type !== "message") {
    return "";
  }
  if (typeof item.content === "string") {
    return item.content;
  }
  return (
    item.parts
      ?.filter((part) => part.type === "text")
      .map((part) => part.text)
      .join("") ?? ""
  );
}

const manager = await FoundryLocalManager.createAsync(managerConfig);
let model;
let modelLoaded = false;

try {
  const cachedModels = await manager.catalog.getCachedModels();
  model = cachedModels.filter(isCpuChatModel).sort(compareModels)[0];
  if (model === undefined) {
    for (const alias of preferredModelNames) {
      let candidate;
      try {
        candidate = await manager.catalog.getModel(alias);
      } catch {
        continue;
      }
      const cpuVariant = candidate.variants.filter(isCpuChatModel).sort(compareModels)[0];
      if (cpuVariant !== undefined) {
        candidate.selectVariant(cpuVariant);
        model = candidate;
        break;
      }
    }
  }
  if (model === undefined) {
    throw new Error("No supported CPU chat model is available.");
  }

  console.log(`Using model: ${model.id}`);
  if (!model.isCached) {
    console.log("Downloading model...");
    await model.download((progress) => process.stdout.write(`\r  ${progress.toFixed(1)}%`));
    process.stdout.write("\n");
  }
  await model.load();
  modelLoaded = true;

  const session = new ChatSession(model);
  try {
    const request = new Request()
      .addItem(Item.systemMessage("Answer accurately with only the requested city name."))
      .addItem(Item.userMessage("What is the capital of France?"))
      .setOptions({
        search: {
          doSample: false,
          maxOutputTokens: 64,
          seed: 42,
          temperature: 0,
        },
      });
    const response = await session.processRequest(request);
    const responseText = response.output.map(extractText).filter(Boolean).join("\n").trim();

    console.log(`Assistant: ${responseText}`);
    if (!responseText.toLowerCase().includes("paris")) {
      throw new Error(`Expected the response to identify Paris, but received: ${responseText}`);
    }
  } finally {
    session.dispose();
  }
} finally {
  try {
    if (modelLoaded) {
      await model.unload();
    }
  } finally {
    manager.dispose();
  }
}
