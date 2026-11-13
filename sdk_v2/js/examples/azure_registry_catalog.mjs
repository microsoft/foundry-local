import { CatalogType, ChatSession, FoundryLocalManager, Item, Request } from "foundry-local-sdk";

const manager = FoundryLocalManager.create({ appName: "RegistryDemo" });
try {
  await manager.downloadAndRegisterEps();
  const catalog = manager.getCatalog(CatalogType.AzureRegistry, {
    name: "azureml-dev",
    credential: "azure_cli",
  });
  const model = await catalog.getModel("qwen2.5-0.5b");
  console.log("Using", model.id);
  await model.download();
  await model.load();
  try {
    const session = new ChatSession(model);
    try {
      const request = new Request().addItem(Item.userMessage("What is 2 + 2? Answer briefly."));
      const response = await session.processRequest(request);
      console.log(response.output
        .filter(item => item.type === "message")
        .flatMap(message => message.parts ?? [Item.text(message.content ?? "")])
        .filter(item => item.type === "text")
        .filter(item => (item.textType ?? "default") === "default")
        .map(item => item.text).join(""));
    } finally {
      session.dispose();
    }
  } finally {
    await model.unload();
  }
} finally {
  manager.dispose();
}
