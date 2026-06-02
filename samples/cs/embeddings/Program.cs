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
var mgr = FoundryLocalManager.Instance;
// </init>

// <model_setup>
// Get the model catalog
var catalog = await mgr.GetCatalogAsync();

// Get an embedding model
var model = await catalog.GetModelAsync("qwen3-embedding-0.6b") ?? throw new Exception("Embedding model not found");

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

using var session = new EmbeddingsSession(model);

// <single_embedding>
Console.WriteLine("\n--- Single Embedding ---");
using (var request = new Request())
{
    request.AddItem(new TextItem("The quick brown fox jumps over the lazy dog"));

    using var response = await session.ProcessRequestAsync(request);
    using var item = response.GetItem(0);
    var tensor = (TensorItem)item;

    // Zero-copy view over the tensor's native buffer — valid only while `item` is alive.
    var values = tensor.AsSpan<float>();
    var preview = Math.Min(5, values.Length);

    Console.WriteLine($"Shape: [{string.Join(", ", tensor.Shape)}] ({values.Length} values)");
    Console.Write($"First {preview}: [");
    for (int i = 0; i < preview; i++)
    {
        Console.Write($"{(i > 0 ? ", " : "")}{values[i]:F6}");
    }
    Console.WriteLine("]");
}
// </single_embedding>

// <batch_embedding>
Console.WriteLine("\n--- Batch Embeddings ---");
using (var request = new Request())
{
    request.AddItem(new TextItem("Machine learning is a subset of artificial intelligence"));
    request.AddItem(new TextItem("The capital of France is Paris"));
    request.AddItem(new TextItem("Rust is a systems programming language"));

    using var response = await session.ProcessRequestAsync(request);
    Console.WriteLine($"Number of embeddings: {response.ItemCount}");
    for (int i = 0; i < response.ItemCount; i++)
    {
        using var item = response.GetItem(i);
        var tensor = (TensorItem)item;
        Console.WriteLine($"  [{i}] Dimensions: [{string.Join(", ", tensor.Shape)}]");
    }
}
// </batch_embedding>

// <cleanup>
// Tidy up - unload the model and dispose the manager so native resources are released promptly.
session.Dispose();
await model.UnloadAsync();
mgr.Dispose();
Console.WriteLine("\nModel unloaded.");
// </cleanup>
// </complete_code>
