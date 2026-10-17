// <complete_code>
// <imports>
using Microsoft.AI.Foundry.Local;
// </imports>

// <init>
var config = new Configuration
{
    AppName = "foundry_local_samples",
    LogLevel = Microsoft.AI.Foundry.Local.LogLevel.Information
};


// Initialize the singleton instance.
await FoundryLocalManager.CreateAsync(config, Utils.GetAppLogger());
using var mgr = FoundryLocalManager.Instance;


// Ensure that any Execution Provider (EP) downloads run and are completed.
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
// </init>


// <model_setup>
// Get the model catalog
var catalog = await mgr.GetCatalogAsync();


// Get a model using an alias and select the CPU model variant
var model = await catalog.GetModelAsync("whisper-tiny") ?? throw new System.Exception("Model not found");
var modelVariant = model.Variants.First(v => v.Info.Runtime?.DeviceType == DeviceType.CPU);
model.SelectVariant(modelVariant);


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


// <transcription>
try
{
    using var session = new AudioSession(model);
    session.SetOptions(new RequestOptions { AdditionalOptions = new Dictionary<string, string> { ["language"] = "en" } });
    session.SetStreaming(true);
    var audioFile = args.Length > 0 ? args[0] : Path.Combine(AppContext.BaseDirectory, "Recording.mp3");
    Console.WriteLine($"Transcribing audio with streaming output: {Path.GetFileName(audioFile)}");
    using var request = new Request();
    request.AddItem(new AudioItem(Path.GetFullPath(audioFile)));
    await using var stream = session.ProcessStreamingRequestAsync(request);
    await foreach (var item in stream)
    {
        using (item)
        {
            if (item is SpeechSegmentItem segment)
            {
                Console.Write(segment.Kind == SpeechSegmentKind.Partial ? $"\r{segment.Text}" : segment.Text);
                Console.Out.Flush();
            }
        }
    }
    Console.WriteLine();
}
// </transcription>


// <cleanup>
// Tidy up - unload the model
finally
{
    await model.UnloadAsync();
}
// </cleanup>
// </complete_code>