using Microsoft.AI.Foundry.Local;
using System.Diagnostics;

CancellationToken ct = new CancellationToken();

var config = new Configuration
{
    AppName = "foundry_local_samples",
    LogLevel = Microsoft.AI.Foundry.Local.LogLevel.Information
};


// Initialize the singleton instance.
await FoundryLocalManager.CreateAsync(config, Utils.GetAppLogger());
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


// Model catalog operations
// In this section of the code we demonstrate the various model catalog operations
// Get the model catalog object
var catalog = await mgr.GetCatalogAsync();

// List available models
Console.WriteLine("Available models for your hardware:");
var models = await catalog.ListModelsAsync();
foreach (var availableModel in models)
{
    foreach (var variant in availableModel.Variants)
    {
        Console.WriteLine($"  - Alias: {variant.Alias} (Id: {string.Join(", ", variant.Id)})");
    }
}

// List cached models (i.e. downloaded models) from the catalog
var cachedModels = await catalog.GetCachedModelsAsync();
Console.WriteLine("\nCached models:");
foreach (var cachedModel in cachedModels)
{
    Console.WriteLine($"- {cachedModel.Alias} ({cachedModel.Id})");
}


// Get a model using an alias from the catalog
var model = await catalog.GetModelAsync("qwen2.5-0.5b") ?? throw new Exception("Model not found");

// Models in Model.Variants are ordered by priority, with the highest priority first.
// The first downloaded model is selected by default.
// The highest priority is selected if no models have been downloaded.
// If the selected variant is not the highest priority, it means that Foundry Local
// has found a locally cached variant for you to improve performance (remove need to download).
Console.WriteLine("\nThe default selected model variant is: " + model.Id);
if (model.Id != model.Variants.First().Id)
{
    Debug.Assert(await model.IsCachedAsync());
    Console.WriteLine("The model variant was selected due to being locally cached.");
}


// OPTIONAL: `model` can be used directly with its currently selected variant.
//           You can explicitly select (`model.SelectVariant`) or use a specific variant from `model.Variants`
//           if you want more control over the device and/or execution provider used.
//
// Choices:
//   - Use a model variant directly from the catalog if you know the variant Id
//     - `var modelVariant = await catalog.GetModelVariantAsync("qwen2.5-0.5b-instruct-generic-gpu:3")`
//
//   - Get the model variant from IModel.Variants
//     - `var modelVariant = model.Variants.First(v => v.Id == "qwen2.5-0.5b-instruct-generic-cpu:4")`
//     - `var modelVariant = model.Variants.First(v => v.Info.Runtime?.DeviceType == DeviceType.GPU)`
//       - optional: update selected variant in `model` using `model.SelectVariant(modelVariant);` if you wish to use
//                   `model` in your code.

// For this example we explicitly select the CPU variant, and call SelectVariant so all the following example code
// uses the `model` instance. It would be equally valid to use `modelVariant` directly.
Console.WriteLine("Selecting CPU variant of model");
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
await model.LoadAsync();


// List loaded models (i.e. in memory) from the catalog
var loadedModels = await catalog.GetLoadedModelsAsync();
Console.WriteLine("\nLoaded models:");
foreach (var loadedModel in loadedModels)
{
    Console.WriteLine($"- {loadedModel.Alias} ({loadedModel.Id})");
}
Console.WriteLine();


// Run a single chat turn to confirm the model is functional.
// See the native-chat-completions sample for streaming and multi-turn examples.
using (var session = new ChatSession(model))
using (var request = new Request())
{
    request.AddItem(MessageItem.User("Why is the sky blue?"));

    Console.WriteLine("Chat completion response:");
    using var response = await session.ProcessRequestAsync(request, ct);
    // Chat responses contain a single MessageItem with a single TextItem part.
    using var item = response.GetItem(0);
    if (item is MessageItem msg)
    {
        Console.WriteLine(msg.GetSimpleText());
    }
}
Console.WriteLine();

// Tidy up - unload the model
Console.WriteLine($"Unloading model {model.Id}...");
await model.UnloadAsync();
Console.WriteLine("Model unloaded.");

// Show loaded models from the catalog after unload
loadedModels = await catalog.GetLoadedModelsAsync();
Console.WriteLine("\nLoaded models after unload (will be empty):");
foreach (var loadedModel in loadedModels)
{
    Console.WriteLine($"- {loadedModel.Alias} ({loadedModel.Id})");
}
Console.WriteLine();
Console.WriteLine("Sample complete.");

// Dispose the manager so native resources are released promptly.
mgr.Dispose();
