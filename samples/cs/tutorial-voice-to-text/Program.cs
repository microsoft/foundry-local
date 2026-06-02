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
    builder.SetMinimumLevel(
        Microsoft.Extensions.Logging.LogLevel.Information
    );
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

var catalog = await mgr.GetCatalogAsync();
// </init>

// <transcription>
// Load the speech-to-text model
var speechModel = await catalog.GetModelAsync("whisper-tiny")
    ?? throw new Exception("Speech model not found");

await speechModel.DownloadAsync(progress =>
{
    Console.Write($"\rDownloading speech model: {progress:F2}%");
    if (progress >= 100f) Console.WriteLine();
});

await speechModel.LoadAsync();
Console.WriteLine("Speech model loaded.");

// Stream output to the console for live feedback, then pull the aggregated transcript
// from the final Response — the downstream summarization needs the consolidated text.
using var audioSession = new AudioSession(speechModel);
audioSession.SetStreaming(true);

// Default to the shared samples/testdata/meeting-notes.wav (copied next to the executable by the csproj).
var audioPath = args.Length > 0
    ? args[0]
    : Path.Combine(AppContext.BaseDirectory, "testdata", "meeting-notes.wav");

Console.WriteLine("\nTranscription:");
string transcription = "";
using (var audioRequest = new Request())
{
    audioRequest.AddItem(new AudioItem(audioPath));

    var stream = audioSession.ProcessStreamingRequestAsync(audioRequest, ct);
    await foreach (var item in stream)
    {
        using (item)
        {
            if (item is TextItem chunk)
            {
                Console.Write(chunk.Text);
            }
        }
    }
    Console.WriteLine();

    using var finalResponse = await stream.FinalResponse;
    // Audio transcription final response contains a single TextItem.
    using var finalItem = finalResponse.GetItem(0);
    if (finalItem is TextItem text)
    {
        transcription = text.Text;
    }
}

// Unload the speech model to free memory
audioSession.Dispose();
await speechModel.UnloadAsync();
// </transcription>

// <summarization>
// Load the chat model for summarization
var chatModel = await catalog.GetModelAsync("qwen2.5-0.5b")
    ?? throw new Exception("Chat model not found");

await chatModel.DownloadAsync(progress =>
{
    Console.Write($"\rDownloading chat model: {progress:F2}%");
    if (progress >= 100f) Console.WriteLine();
});

await chatModel.LoadAsync();
Console.WriteLine("Chat model loaded.");

// Summarize the transcription into organized notes.
using var chatSession = new ChatSession(chatModel);
using var chatRequest = new Request();
chatRequest.AddItem(MessageItem.System(
    "You are a note-taking assistant. " +
    "Summarize the following transcription into organized, concise notes with bullet points."));
chatRequest.AddItem(MessageItem.User(transcription));

using var chatResponse = await chatSession.ProcessRequestAsync(chatRequest, ct);
Console.WriteLine("\nSummary:");
// Chat responses contain a single MessageItem with a single TextItem part.
using var summaryItem = chatResponse.GetItem(0);
if (summaryItem is MessageItem msg)
{
    Console.WriteLine(msg.GetSimpleText());
}

// Clean up - unload the model and dispose the manager so native resources are released promptly.
chatSession.Dispose();
await chatModel.UnloadAsync();
mgr.Dispose();
Console.WriteLine("\nDone. Models unloaded.");
// </summarization>
// </complete_code>
