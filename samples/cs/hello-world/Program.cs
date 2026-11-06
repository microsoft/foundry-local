using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging.Abstractions;

var configuration = new Configuration
{
    AppName = "foundry_local_samples",
    LogLevel = LogLevel.Information,
};

await FoundryLocalManager.CreateAsync(configuration, NullLogger.Instance).ConfigureAwait(false);
using var manager = FoundryLocalManager.Instance;

Console.WriteLine("Discovering execution providers:");
var executionProviders = manager.DiscoverEps();
foreach (var executionProvider in executionProviders)
{
    Console.WriteLine($"  {executionProvider.Name} (registered: {executionProvider.IsRegistered})");
}

Console.WriteLine("Downloading and registering execution providers...");
await manager.DownloadAndRegisterEpsAsync(
    (name, progress) => Console.Write($"\r  {name}: {progress:F1}%   "))
    .ConfigureAwait(false);
Console.WriteLine();

Console.WriteLine("Opening the public catalog...");
var catalog = await manager.GetCatalogAsync().ConfigureAwait(false);
var model = await catalog.GetModelAsync("qwen2.5-0.5b").ConfigureAwait(false)
    ?? throw new InvalidOperationException("The qwen2.5-0.5b model was not found.");

if (!await model.IsCachedAsync().ConfigureAwait(false))
{
    Console.WriteLine($"Downloading {model.Id}...");
    await model.DownloadAsync(
        progress => Console.Write($"\r  {progress:F1}%   "))
        .ConfigureAwait(false);
    Console.WriteLine();
}
else
{
    Console.WriteLine($"{model.Id} is already cached; skipping download.");
}

Console.WriteLine($"Loading {model.Id}...");
await model.LoadAsync().ConfigureAwait(false);

try
{
    using var session = new ChatSession(model);
    session.SetStreaming(true);
    using var request = new Request();
    request.AddItem(MessageItem.User("Why is the sky blue? Answer in two sentences."));
    request.SetOptions(new RequestOptions
    {
        Search = new SearchOptions { Temperature = 0.0f, MaxOutputTokens = 128 },
    });

    Console.Write("Assistant: ");
    await using var stream = session.ProcessStreamingRequestAsync(request);
    await foreach (var item in stream.ConfigureAwait(false))
    {
        using (item)
        {
            if (item is TextItem { Type: TextItemType.Default } text)
            {
                Console.Write(text.Text);
                Console.Out.Flush();
            }
        }
    }

    using var response = await stream.FinalResponse.ConfigureAwait(false);
    Console.WriteLine();
}
finally
{
    Console.WriteLine($"Unloading {model.Id}...");
    await model.UnloadAsync().ConfigureAwait(false);
}
