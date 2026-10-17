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
using var mgr = FoundryLocalManager.Instance;

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
Console.WriteLine("Model loaded and ready.");

try
{
    using var session = new ChatSession(model);
    session.SetStreaming(true);
    // </init>

    // <system_prompt>
    // Start the conversation with a system prompt
    var firstTurn = true;
    const string systemPrompt = "You are a helpful, friendly assistant. Keep your responses " +
        "concise and conversational. If you don't know something, say so.";
    // </system_prompt>

    Console.WriteLine("\nChat assistant ready! Type 'quit' to exit.\n");

    // <conversation_loop>
    while (true)
    {
        Console.Write("You: ");
        var userInput = Console.ReadLine();
        if (string.IsNullOrWhiteSpace(userInput) ||
            userInput.Equals("quit", StringComparison.OrdinalIgnoreCase) ||
            userInput.Equals("exit", StringComparison.OrdinalIgnoreCase))
        {
            break;
        }

        // The session retains completed turns; send only new input.
        using var request = new Request();
        if (firstTurn)
        {
            request.AddItem(MessageItem.System(systemPrompt));
        }
        request.AddItem(MessageItem.User(userInput));

        // <streaming>
        // Stream the response token by token
        Console.Write("Assistant: ");
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
        Console.WriteLine("\n");
        // </streaming>

        firstTurn = false;
    }
}
// </conversation_loop>

// Clean up - unload the model
finally
{
    await model.UnloadAsync();
}
Console.WriteLine("Model unloaded. Goodbye!");
// </complete_code>
