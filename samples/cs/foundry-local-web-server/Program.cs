// <complete_code>
// <imports>
using Microsoft.AI.Foundry.Local;
using OpenAI;
using System.ClientModel;
// </imports>

// <init>
var config = new Configuration
{
    AppName = "foundry_local_samples",
    LogLevel = Microsoft.AI.Foundry.Local.LogLevel.Information,
    Web = new Configuration.WebService
    {
        Urls = "http://127.0.0.1:52495"
    }
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


// Get a model using an alias
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


// <server_setup>
// Start the web service
Console.Write($"Starting web service on {config.Web.Urls}...");
await mgr.StartWebServiceAsync();
Console.WriteLine("done.");

// <<<<<< OPEN AI SDK USAGE >>>>>>
// Use the OpenAI SDK to call the local Foundry web service

ApiKeyCredential key = new ApiKeyCredential("notneeded");
OpenAIClient client = new OpenAIClient(key, new OpenAIClientOptions
{
    Endpoint = new Uri(config.Web.Urls + "/v1"),
});

var chatClient = client.GetChatClient(model.Id);
var completionUpdates = chatClient.CompleteChatStreaming("Why is the sky blue?");

Console.Write($"[ASSISTANT]: ");
foreach (var completionUpdate in completionUpdates)
{
    if (completionUpdate.ContentUpdate.Count > 0)
    {
        Console.Write(completionUpdate.ContentUpdate[0].Text);
    }
}
Console.WriteLine();
// <<<<<< END OPEN AI SDK USAGE >>>>>>

// Tidy up
// Stop the web service and unload model
await mgr.StopWebServiceAsync();
await model.UnloadAsync();
// </server_setup>
// </complete_code>