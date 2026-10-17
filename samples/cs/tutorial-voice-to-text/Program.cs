// <complete_code>
// <imports>
using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging;
using System.Text;
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

// Transcribe the audio file
var transcriptionText = new StringBuilder();

try
{
    using var session = new AudioSession(speechModel);
    session.SetStreaming(true);
    using var request = new Request();
    request.AddItem(new AudioItem(Path.GetFullPath(args.Length > 0 ? args[0] : "meeting-notes.wav")));
    Console.WriteLine("\nTranscription:");
    await using var stream = session.ProcessStreamingRequestAsync(request, ct);
    await foreach (var item in stream)
    {
        using (item)
        {
            if (item is SpeechSegmentItem segment)
            {
                Console.Write(segment.Kind == SpeechSegmentKind.Partial ? $"\r{segment.Text}" : segment.Text);
            }
        }
    }
    using var response = await stream.FinalResponse;
    transcriptionText.Append(response.OfType<SpeechResultItem>().Single().Text);
    Console.WriteLine();
}

// Unload the speech model to free memory
finally
{
    await speechModel.UnloadAsync();
}
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

// Summarize the transcription into organized notes
try
{
    using var session = new ChatSession(chatModel);
    using var request = new Request();
    request.AddItem(MessageItem.System("You are a note-taking assistant. Summarize " +
        "the following transcription into organized, concise notes with bullet points."));
    request.AddItem(MessageItem.User(transcriptionText.ToString()));
    using var response = await session.ProcessRequestAsync(request, ct);
    Console.WriteLine($"\nSummary:\n{Utils.GetResponseText(response)}");
}

// Clean up
finally
{
    await chatModel.UnloadAsync();
}
Console.WriteLine("\nDone. Models unloaded.");
// </summarization>
// </complete_code>
