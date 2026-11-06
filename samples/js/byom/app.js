import { resolve } from "node:path";

import { CatalogType, ChatSession, FoundryLocalManager, Item, MutableModelInfo, Request } from "foundry-local-sdk";

const modelDirectory = process.argv[2];
if (modelDirectory === undefined) {
  throw new Error("Usage: node app.js <model-directory>");
}

const manager = FoundryLocalManager.create({ appName: "foundry_local_samples" });
const metadata = new MutableModelInfo().setStringProperty("task", "chat-completion");
let catalog;
let model;
let session;
let modelLoaded = false;

try {
  await manager.downloadAndRegisterEps();
  catalog = manager.getCatalog(CatalogType.Local);
  model = await catalog.registerModel(resolve(modelDirectory), "sample-byom:1", metadata);
  await model.load();
  modelLoaded = true;

  session = new ChatSession(model);
  const request = new Request().addItem(Item.userMessage("Why is the sky blue? Answer in two sentences."));
  const response = await session.processRequest(request);
  for (const item of response.output) {
    if (item.type === "message") {
      const text = item.parts
        .filter((part) => part.type === "text" && part.textType === "default")
        .map((part) => part.text).join("");
      console.log(`Assistant: ${text}`);
    }
  }
} finally {
  session?.dispose();
  try {
    if (modelLoaded) {
      await model.unload();
    }
    if (model !== undefined) {
      await catalog.unregisterModel("sample-byom:1");
    }
  } finally {
    metadata.dispose();
    manager.dispose();
  }
}
