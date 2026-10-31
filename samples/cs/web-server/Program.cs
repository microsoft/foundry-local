using System.ClientModel;
using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging.Abstractions;
using OpenAI;

var configuration = new Configuration
{
    AppName = "foundry_local_samples",
    LogLevel = LogLevel.Information,
    Web = new Configuration.WebService
    {
        Urls = "http://127.0.0.1:0",
    },
};

await FoundryLocalManager.CreateAsync(configuration, NullLogger.Instance).ConfigureAwait(false);
using var manager = FoundryLocalManager.Instance;
await manager.DownloadAndRegisterEpsAsync().ConfigureAwait(false);

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

await model.LoadAsync().ConfigureAwait(false);

try
{
    await manager.StartWebServiceAsync().ConfigureAwait(false);

    try
    {
        var discoveredUrl = manager.Urls?.FirstOrDefault()
            ?? throw new InvalidOperationException("The built-in server did not report a bound URL.");
        var endpoint = new Uri($"{discoveredUrl.TrimEnd('/')}/v1");
        Console.WriteLine($"Built-in server: {endpoint}");

        var client = new OpenAIClient(
            new ApiKeyCredential("not-needed"),
            new OpenAIClientOptions { Endpoint = endpoint });
        var chatClient = client.GetChatClient(model.Id);

        Console.Write("Assistant: ");
        await foreach (var update in chatClient.CompleteChatStreamingAsync(
            "Why is the sky blue? Answer in one sentence.").ConfigureAwait(false))
        {
            foreach (var part in update.ContentUpdate)
            {
                Console.Write(part.Text);
            }
        }
        Console.WriteLine();
    }
    finally
    {
        await manager.StopWebServiceAsync().ConfigureAwait(false);
    }
}
finally
{
    await model.UnloadAsync().ConfigureAwait(false);
}
