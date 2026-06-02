// <complete_code>
// <imports>
using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging;
// </imports>

// <init>
CancellationToken ct = CancellationToken.None;

var config = new Configuration
{
    AppName = "foundry_local_samples",
    LogLevel = Microsoft.AI.Foundry.Local.LogLevel.Information
};

using var loggerFactory = LoggerFactory.Create(builder =>
{
    builder.SetMinimumLevel(Microsoft.Extensions.Logging.LogLevel.Information);
});
var logger = loggerFactory.CreateLogger<Program>();

// Initialize the singleton instance
await FoundryLocalManager.CreateAsync(config, logger);
var mgr = FoundryLocalManager.Instance;

// Download and register all execution providers.
var currentEp = "";
await mgr.DownloadAndRegisterEpsAsync((epName, percent) =>
{
    if (epName != currentEp)
    {
        if (currentEp != "") Console.WriteLine();
        currentEp = epName;
    }
    Console.Write($"\r  {epName.PadRight(30)}  {percent,6:F1}%");
});
if (currentEp != "") Console.WriteLine();

// Select and load a model from the catalog
var catalog = await mgr.GetCatalogAsync();
var model = await catalog.GetModelAsync("qwen2.5-0.5b")
    ?? throw new Exception("Model not found");

await model.DownloadAsync(progress =>
{
    Console.Write($"\rDownloading model: {progress:F2}%");
    if (progress >= 100f) Console.WriteLine();
});

await model.LoadAsync();
Console.WriteLine("Model loaded and ready.\n");
// </init>

// <summarization>
var systemPrompt =
    "Summarize the following document into concise bullet points. " +
    "Focus on the key points and main ideas.";

// <file_reading>
// Default to the shared samples/testdata/document.txt (copied next to the executable by the csproj).
var defaultDocument = Path.Combine(AppContext.BaseDirectory, "testdata", "document.txt");
var target = args.Length > 0 ? args[0] : defaultDocument;
// </file_reading>

if (Directory.Exists(target))
{
    await SummarizeDirectoryAsync(model, target, systemPrompt, ct);
}
else
{
    Console.WriteLine($"--- {Path.GetFileName(target)} ---");
    await SummarizeFileAsync(model, target, systemPrompt, ct);
}
// </summarization>

// Clean up - unload the model and dispose the manager so native resources are released promptly.
await model.UnloadAsync();
mgr.Dispose();
Console.WriteLine("\nModel unloaded. Done!");

async Task SummarizeFileAsync(
    IModel model,
    string filePath,
    string prompt,
    CancellationToken token)
{
    var fileContent = await File.ReadAllTextAsync(filePath, token);

    // Fresh session per file so summaries don't leak across documents.
    using var session = new ChatSession(model);
    using var request = new Request();
    request.AddItem(MessageItem.System(prompt));
    request.AddItem(MessageItem.User(fileContent));

    using var response = await session.ProcessRequestAsync(request, token);
    // Chat responses contain a single MessageItem with a single TextItem part.
    using var item = response.GetItem(0);
    if (item is MessageItem msg)
    {
        Console.WriteLine(msg.GetSimpleText());
    }
}

async Task SummarizeDirectoryAsync(
    IModel model,
    string directory,
    string prompt,
    CancellationToken token)
{
    var txtFiles = Directory.GetFiles(directory, "*.txt")
        .OrderBy(f => f)
        .ToArray();

    if (txtFiles.Length == 0)
    {
        Console.WriteLine($"No .txt files found in {directory}");
        return;
    }

    foreach (var txtFile in txtFiles)
    {
        Console.WriteLine($"--- {Path.GetFileName(txtFile)} ---");
        await SummarizeFileAsync(model, txtFile, prompt, token);
        Console.WriteLine();
    }
}
// </complete_code>
