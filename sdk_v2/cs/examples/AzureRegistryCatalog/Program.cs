// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging.Abstractions;

await FoundryLocalManager.CreateAsync(new Configuration { AppName = "RegistryDemo" }, NullLogger.Instance);
using var manager = FoundryLocalManager.Instance;
var result = await manager.DownloadAndRegisterEpsAsync();
if (!result.Success)
{
    throw new InvalidOperationException(result.Status);
}
var catalog = await manager.GetCatalogAsync(CatalogType.AzureRegistry,
    new Dictionary<string, string> { ["name"] = "azureml-dev", ["credential"] = "azure_cli" });
foreach (var model in await catalog.ListModelsAsync())
{
    foreach (var variant in model.Variants)
    {
        Console.WriteLine($"- {model.Alias} {variant.Id}");
    }
}
