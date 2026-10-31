using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging.Abstractions;

var configuration = new Configuration
{
    AppName = "foundry_local_samples",
    LogLevel = LogLevel.Information,
};

await FoundryLocalManager.CreateAsync(configuration, NullLogger.Instance).ConfigureAwait(false);
using var manager = FoundryLocalManager.Instance;
await manager.DownloadAndRegisterEpsAsync().ConfigureAwait(false);

var catalog = await manager.GetCatalogAsync().ConfigureAwait(false);
var model = await catalog.GetModelAsync("qwen3-embedding-0.6b").ConfigureAwait(false)
    ?? throw new InvalidOperationException("The qwen3-embedding-0.6b model was not found.");

if (!await model.IsCachedAsync().ConfigureAwait(false))
{
    Console.WriteLine($"Downloading {model.Id}...");
    await model.DownloadAsync(
        progress => Console.Write($"\r  {progress:F1}%   "))
        .ConfigureAwait(false);
    Console.WriteLine();
}

await model.LoadAsync().ConfigureAwait(false);

try
{
    using var session = new EmbeddingsSession(model);
    using var request = new Request();
    string[] inputs =
    [
        "A kitten is sleeping on a rug.",
        "A cat rests on a mat.",
        "Saturn has a prominent ring system.",
    ];
    foreach (var input in inputs)
    {
        request.AddItem(new TextItem(input));
    }

    using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);
    var embeddings = new List<float[]>();
    for (var index = 0; index < response.ItemCount; index++)
    {
        using var item = response.GetItem(index);
        if (item is not TensorItem tensor)
        {
            throw new InvalidOperationException($"Expected a tensor at index {index}.");
        }

        embeddings.Add(tensor.ToArray<float>());
    }

    Console.WriteLine($"Model: {model.Id}");
    Console.WriteLine($"Dimensions: {embeddings[0].Length}");
    Console.WriteLine($"Similarity (cat, cat): {CosineSimilarity(embeddings[0], embeddings[1])}");
    Console.WriteLine($"Similarity (cat, Saturn): {CosineSimilarity(embeddings[0], embeddings[2])}");
}
finally
{
    await model.UnloadAsync().ConfigureAwait(false);
}

// EmbeddingsSession returns L2-normalized vectors, so their dot product is cosine similarity.
static float CosineSimilarity(float[] left, float[] right)
    => left.Zip(right, (a, b) => a * b).Sum();
