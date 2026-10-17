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
using var mgr = FoundryLocalManager.Instance;


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
try
{
    using var session = new ChatSession(model);
    session.SetStreaming(true);
    using var request = new Request();
    request.AddItem(MessageItem.User("Why is the sky blue?"));

    Console.WriteLine("Chat completion response:");
    await using var stream = session.ProcessStreamingRequestAsync(request, ct);
    await foreach (var item in stream)
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
    Console.WriteLine();
}
// </chat_completion>

// <cleanup>
finally
{
    await model.UnloadAsync();
}
// </cleanup>
// </complete_code>