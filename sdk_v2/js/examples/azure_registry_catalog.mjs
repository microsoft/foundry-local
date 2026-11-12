import { CatalogType, FoundryLocalManager } from "foundry-local-sdk";

const manager = FoundryLocalManager.create({ appName: "RegistryDemo" });
try {
  await manager.downloadAndRegisterEps();
  const catalog = manager.getCatalog(CatalogType.AzureRegistry, {
    name: "azureml-dev",
    credential: "azure_cli",
  });
  for (const model of await catalog.getModels()) {
    for (const variant of model.variants) {
      console.log("-", model.alias, variant.id);
    }
  }
} finally {
  manager.dispose();
}
