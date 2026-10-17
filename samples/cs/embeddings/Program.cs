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

// <single_embedding>
try
{
    using var session = new EmbeddingsSession(model);

    // Generate a single embedding
    Console.WriteLine("\n--- Single Embedding ---");
    using var request = new Request();
    request.AddItem(new TextItem("The quick brown fox jumps over the lazy dog"));
    using var response = await session.ProcessRequestAsync(request);
    var embedding = response.OfType<TensorItem>().Single().ToArray<float>();
    Console.WriteLine($"Dimensions: {embedding.Length}");
    Console.WriteLine($"First 5 values: [{string.Join(", ", embedding.Take(5).Select(v => v.ToString("F6")))}]");
    // </single_embedding>

    // <batch_embedding>
    // Generate embeddings for multiple inputs
    Console.WriteLine("\n--- Batch Embeddings ---");
    string[] inputs = [
        "Machine learning is a subset of artificial intelligence",
    "The capital of France is Paris",
    "Rust is a systems programming language"
    ];
    using var batchRequest = new Request();
    foreach (var input in inputs)
    {
        batchRequest.AddItem(new TextItem(input));
    }
    using var batchResponse = await session.ProcessRequestAsync(batchRequest);
    var tensors = batchResponse.OfType<TensorItem>().ToArray();

    Console.WriteLine($"Number of embeddings: {tensors.Length}");
    for (var i = 0; i < tensors.Length; i++)
    {
        Console.WriteLine($"  [{i}] Dimensions: {tensors[i].ElementCount}");
    }
}
// </batch_embedding>

// <cleanup>
// Tidy up - unload the model
finally
{
    await model.UnloadAsync();
}
Console.WriteLine("\nModel unloaded.");
// </cleanup>
// </complete_code>
