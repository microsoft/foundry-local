// <complete_code>
// <imports>
using Microsoft.AI.Foundry.Local;
// </imports>

// <init>
CancellationToken ct = new CancellationToken();

var config = new Configuration
{
    AppName = "foundry_local_samples",
    LogLevel = Microsoft.AI.Foundry.Local.LogLevel.Information
};


// Initialize the singleton instance.
await FoundryLocalManager.CreateAsync(config, Utils.GetAppLogger());
var mgr = FoundryLocalManager.Instance;


// Discover available execution providers and their registration status.
var eps = mgr.DiscoverEps();
int maxNameLen = 30;
Console.WriteLine("Available execution providers:");
Console.WriteLine($"  {"Name".PadRight(maxNameLen)}  Registered");
Console.WriteLine($"  {new string('─', maxNameLen)}  {"──────────"}");
foreach (var ep in eps)
{
    Console.WriteLine($"  {ep.Name.PadRight(maxNameLen)}  {ep.IsRegistered}");
}

// Download and register all execution providers with per-EP progress.
// EP packages include dependencies and may be large.
// Download is only required again if a new version of the EP is released.
// For cross platform builds there is no dynamic EP download and this will return immediately.
Console.WriteLine("\nDownloading execution providers:");
if (eps.Length > 0)
{
    string currentEp = "";
    await mgr.DownloadAndRegisterEpsAsync((epName, percent) =>
    {
        if (epName != currentEp)
        {
            if (currentEp != "")
            {
                Console.WriteLine();
            }
            currentEp = epName;
        }
        Console.Write($"\r  {epName.PadRight(maxNameLen)}  {percent,6:F1}%");
    });
    Console.WriteLine();
}
else
{
    Console.WriteLine("No execution providers to download.");
}
// </init>


// <model_setup>
// Get the model catalog
var catalog = await mgr.GetCatalogAsync();


// Get a model using an alias.
var model = await catalog.GetModelAsync("qwen2.5-0.5b") ?? throw new Exception("Model not found");

// Download the model (the method skips download if already cached)
await model.DownloadAsync(progress =>
{
    Console.Write($"\rDownloading model: {progress:F2}%");
    if (progress >= 100f)
    {
        Console.WriteLine();
    }
});

// Load the model
Console.Write($"Loading model {model.Id}...");
await model.LoadAsync();
Console.WriteLine("done.");
// </model_setup>

// <chat_completion>
{
    using var session = new ChatSession(model);

    // Turn 1 — non-streaming. Include a system message to steer the assistant.
    using var request1 = new Request();
    request1.AddItem(MessageItem.System("You are a concise science tutor. Answer in 1-2 sentences."));
    request1.AddItem(MessageItem.User("Why is the sky blue?"));

    Console.WriteLine("Turn 1 (non-streaming):");
    using (var response1 = await session.ProcessRequestAsync(request1, ct))
    {
        using var item = response1.GetItem(0);
        if (item is MessageItem msg)
        {
            Console.WriteLine(msg.GetSimpleText());
        }
    }

    // Turn 2 — streaming. Session retains history — only the new turn is sent.
    session.SetStreaming(true);
    using var request2 = new Request();
    request2.AddItem(MessageItem.User("And why are sunsets red?"));

    Console.WriteLine("\nTurn 2 (streaming):");
    await foreach (var item in session.ProcessStreamingRequestAsync(request2, ct))
    {
        using (item)
        {
            if (item is TextItem txt)
            {
                Console.Write(txt.Text);
                Console.Out.Flush();
            }
        }
    }
    Console.WriteLine();

    Console.WriteLine($"\nCompleted turns in session: {session.TurnCount}");
}
// </chat_completion>

// <cleanup>
// Tidy up - unload the model and dispose the manager so native resources are released promptly.
await model.UnloadAsync();
mgr.Dispose();
// </cleanup>
// </complete_code>