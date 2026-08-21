// <complete_code>
// <imports>
using Microsoft.AI.Foundry.Local;
using Betalgo.Ranul.OpenAI.ObjectModels.RequestModels;
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

// Get a chat client
var chatClient = await model.GetChatClientAsync();
// </init>

// <summarization>
var systemPrompt1 =
    "Summarize the following document into concise bullet points. " +
    "Focus on the key points and main ideas.";

var systemPrompt2 =
    "Summarize the following document in a single, concise paragraph. " +
    "Capture the main argument and supporting points.";

var systemPrompt3 =
    "Extract the three most important takeaways from the following document. " +
    "Number each takeaway and keep each to one or two sentences.";

// <file_reading>
var target = args.Length > 0 ? args[0] : Path.Combine(AppContext.BaseDirectory, "document.txt");
// </file_reading>

if (Directory.Exists(target))
{
    await SummarizeDirectoryAsync(chatClient, target, systemPrompt1, ct);
}
else
{
    Console.WriteLine($"--- {Path.GetFileName(target)} ---");
    await SummarizeFileAsync(chatClient, target, systemPrompt1, ct);
}
// </summarization>

// Clean up
await model.UnloadAsync();
Console.WriteLine("\nModel unloaded. Done!");

async Task SummarizeFileAsync(
    dynamic client,
    string filePath,
    string prompt,
    CancellationToken token)
{
    var fileContent = await File.ReadAllTextAsync(filePath, token);
    var messages = new List<ChatMessage>
    {
        new ChatMessage { Role = "system", Content = prompt },
        new ChatMessage { Role = "user", Content = fileContent }
    };

    var response = await client.CompleteChatAsync(messages, token);
    Console.WriteLine(response.Choices[0].Message.Content);
}

async Task SummarizeDirectoryAsync(
    dynamic client,
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
        await SummarizeFileAsync(client, txtFile, prompt, token);
        Console.WriteLine();
    }
}
// </complete_code>
