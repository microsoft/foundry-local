using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging.Abstractions;

if (args.Length != 1)
{
    throw new ArgumentException("Usage: byom <model-directory>");
}

var configuration = new Configuration { AppName = "foundry_local_samples" };
await FoundryLocalManager.CreateAsync(configuration, NullLogger.Instance).ConfigureAwait(false);
using var manager = FoundryLocalManager.Instance;
await manager.DownloadAndRegisterEpsAsync().ConfigureAwait(false);

var catalog = await manager.GetCatalogAsync(CatalogType.Local).ConfigureAwait(false);
var metadata = new ModelInfoBuilder().SetStringProperty(ModelInfoPropertyKeys.Task, "chat-completion");
var model = await catalog.RegisterModelAsync(
    Path.GetFullPath(args[0]), "sample-byom:1", metadata).ConfigureAwait(false);

try
{
    await model.LoadAsync().ConfigureAwait(false);
    using var session = new ChatSession(model);
    using var request = new Request();
    request.AddItem(MessageItem.User("Why is the sky blue? Answer in two sentences."));

    using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);
    foreach (var item in response)
    {
        using (item)
        {
            if (item is MessageItem message)
            {
                var text = string.Concat(message.Parts.OfType<TextItem>()
                    .Where(part => part.Type == TextItemType.Default).Select(part => part.Text));
                Console.WriteLine($"Assistant: {text}");
            }
        }
    }
}
finally
{
    if (await model.IsLoadedAsync().ConfigureAwait(false))
    {
        await model.UnloadAsync().ConfigureAwait(false);
    }
    await catalog.UnregisterModelAsync("sample-byom:1").ConfigureAwait(false);
}
