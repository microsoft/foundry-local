using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging.Abstractions;

if (args.Length != 2)
{
    throw new ArgumentException("Usage: vision <model-alias> <image-path>");
}

var configuration = new Configuration { AppName = "foundry_local_samples" };
await FoundryLocalManager.CreateAsync(configuration, NullLogger.Instance).ConfigureAwait(false);
using var manager = FoundryLocalManager.Instance;
await manager.DownloadAndRegisterEpsAsync().ConfigureAwait(false);

var catalog = await manager.GetCatalogAsync().ConfigureAwait(false);
var model = await catalog.GetModelAsync(args[0]).ConfigureAwait(false)
    ?? throw new InvalidOperationException($"Model {args[0]} was not found.");
if (!await model.IsCachedAsync().ConfigureAwait(false))
{
    await model.DownloadAsync().ConfigureAwait(false);
}
await model.LoadAsync().ConfigureAwait(false);

try
{
    using var session = new ChatSession(model);
    using var request = new Request();
    request.AddItem(new MessageItem(MessageRole.User,
    [
        new TextItem("Describe this image in one short sentence."),
        new ImageItem(Path.GetFullPath(args[1])),
    ]));

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
    await model.UnloadAsync().ConfigureAwait(false);
}
