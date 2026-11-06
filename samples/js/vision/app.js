import { resolve } from "node:path";

import { ChatSession, FoundryLocalManager, Item, Request } from "foundry-local-sdk";

const [modelAlias, imagePath] = process.argv.slice(2);
if (modelAlias === undefined || imagePath === undefined) {
  throw new Error("Usage: node app.js <model-alias> <image-path>");
}

const manager = FoundryLocalManager.create({ appName: "foundry_local_samples" });
let model;
let session;
let modelLoaded = false;

try {
  await manager.downloadAndRegisterEps();
  model = await manager.catalog.getModel(modelAlias);
  if (!model.isCached) {
    await model.download();
  }
  await model.load();
  modelLoaded = true;

  session = new ChatSession(model);
  const request = new Request().addItem(Item.userMessage([
    Item.text("Describe this image in one short sentence."),
    Item.imageFromUri(resolve(imagePath)),
  ]));
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
  } finally {
    manager.dispose();
  }
}
