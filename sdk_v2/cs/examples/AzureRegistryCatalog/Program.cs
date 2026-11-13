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
var model = await catalog.GetModelAsync("qwen2.5-0.5b")
    ?? throw new InvalidOperationException("qwen2.5-0.5b is not available in this registry");
Console.WriteLine($"Using {model.Id}");
await model.DownloadAsync();
await model.LoadAsync();
try
{
    using var session = new ChatSession(model);
    using var request = new Request();
    request.AddItem(MessageItem.User("What is 2 + 2? Answer briefly."));
    using var response = await session.ProcessRequestAsync(request);
    Console.WriteLine(string.Concat(response.OfType<MessageItem>()
        .SelectMany(message => message.Parts).OfType<TextItem>()
        .Where(text => text.Type == TextItemType.Default).Select(text => text.Text)));
}
finally
{
    await model.UnloadAsync();
}
