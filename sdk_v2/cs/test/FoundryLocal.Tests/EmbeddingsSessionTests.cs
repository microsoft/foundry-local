// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Runtime.InteropServices;
using System.Threading.Tasks;

using Microsoft.AI.Foundry.Local.Detail.Interop;

using TUnit.Core.Exceptions;

#pragma warning disable CA2000 // Items are transferred to Request via AddItem

[SkipUnlessIntegration]
internal sealed class EmbeddingsSessionTests
{
    private static IModel? model;

    [Before(Class)]
    public static async Task Setup()
    {
        try
        {
            var manager = FoundryLocalManager.Instance;
            var catalog = await manager.GetCatalogAsync();

            // Pick the smallest CPU embedding model from the catalog.
            var allModels = await catalog.ListModelsAsync().ConfigureAwait(false);
            var embeddingModel = allModels
                .Where(m => m.Info.Task == "embeddings"
                            && m.Info.Runtime?.DeviceType == DeviceType.CPU)
                .OrderBy(m => m.Info.FileSizeMb ?? int.MaxValue)
                .FirstOrDefault();

            if (embeddingModel == null)
            {
                return; // No embedding model in catalog — tests will skip individually
            }

            if (!await embeddingModel.IsCachedAsync())
            {
                return;
            }

            await embeddingModel.LoadAsync().ConfigureAwait(false);
            await Assert.That(await embeddingModel.IsLoadedAsync()).IsTrue();

            EmbeddingsSessionTests.model = embeddingModel;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"Setup failed: {ex}");
            throw;
        }
    }

    [Test]
    public async Task Embed_SingleInput_ReturnsOneTensor()
    {
        if (model == null)
        {
            throw new SkipTestException("Embeddings model not available");
        }

        using var session = new EmbeddingsSession(model!);

        using var request = new Request();
        request.AddItem(new TextItem("hello world"));

        using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);

        await Assert.That(response).IsNotNull();
        await Assert.That(response.ItemCount).IsEqualTo(1);

        using var item = response.GetItem(0);
        await Assert.That(item).IsTypeOf<TensorItem>();

        var tensor = (TensorItem)item;
        await Assert.That(tensor.DataType).IsEqualTo(FlTensorDataType.Float);
        await Assert.That(tensor.Shape.Length).IsGreaterThanOrEqualTo(1);

        long totalElements = 1;
        foreach (var dim in tensor.Shape)
        {
            totalElements *= dim;
        }

        await Assert.That(totalElements).IsGreaterThan(0);

        // Read the float data to verify it's accessible
        var floats = new float[totalElements];
        Marshal.Copy(tensor.Data, floats, 0, (int)totalElements);

        // At least some values should be non-zero for a meaningful embedding
        await Assert.That(floats.Any(f => f != 0.0f)).IsTrue();

        Console.WriteLine($"Embedding dimensions: {string.Join("x", tensor.Shape)}, first value: {floats[0]}");
    }

    [Test]
    public async Task Embed_BatchInput_ReturnsMatchingTensorCount()
    {
        if (model == null)
        {
            throw new SkipTestException("Embeddings model not available");
        }

        using var session = new EmbeddingsSession(model!);

        using var request = new Request();
        request.AddItem(new TextItem("first sentence"));
        request.AddItem(new TextItem("second sentence"));
        request.AddItem(new TextItem("third sentence"));

        using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);

        await Assert.That(response).IsNotNull();
        await Assert.That(response.ItemCount).IsEqualTo(3);

        long? firstDimensionality = null;

        for (int i = 0; i < response.ItemCount; i++)
        {
            using var item = response.GetItem(i);
            await Assert.That(item).IsTypeOf<TensorItem>();

            var tensor = (TensorItem)item;
            await Assert.That(tensor.DataType).IsEqualTo(FlTensorDataType.Float);

            long totalElements = 1;
            foreach (var dim in tensor.Shape)
            {
                totalElements *= dim;
            }

            if (firstDimensionality == null)
            {
                firstDimensionality = totalElements;
            }
            else
            {
                // All embeddings from the same model should have the same dimensionality
                await Assert.That(totalElements).IsEqualTo(firstDimensionality.Value);
            }
        }

        Console.WriteLine($"Batch: 3 inputs → 3 tensors, each with {firstDimensionality} elements");
    }

    [Test]
    public async Task Embed_EmptyRequest_HandledGracefully()
    {
        if (model == null)
        {
            throw new SkipTestException("Embeddings model not available");
        }

        using var session = new EmbeddingsSession(model!);

        using var request = new Request();

        // An empty request should either return an empty response or throw FoundryLocalException.
        // It must not crash or hang.
        try
        {
            using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);

            // If we get here, the response should be empty or have zero items.
            await Assert.That(response).IsNotNull();
            await Assert.That(response.ItemCount).IsEqualTo(0);
            Console.WriteLine("Empty request returned empty response");
        }
        catch (FoundryLocalException ex)
        {
            // Also acceptable — the engine rejected the empty request.
            await Assert.That(ex).IsNotNull();
            Console.WriteLine($"Empty request threw FoundryLocalException: {ex.Message}");
        }
    }

    [Test]
    public async Task Embed_WrongModelTask_ThrowsArgumentException()
    {
        // Use the chat model to verify EmbeddingsSession rejects it.
        var manager = FoundryLocalManager.Instance;
        var catalog = await manager.GetCatalogAsync();

        var chatModel = await catalog.GetModelVariantAsync("qwen2.5-0.5b-instruct-generic-cpu:4")
            .ConfigureAwait(false);

        if (chatModel == null)
        {
            throw new SkipTestException("Chat model not available for wrong-task test");
        }

        ArgumentException? caught = null;

        try
        {
            using var session = new EmbeddingsSession(chatModel);
        }
        catch (ArgumentException ex)
        {
            caught = ex;
        }

        await Assert.That(caught).IsNotNull();
        Console.WriteLine($"Correctly rejected chat model: {caught!.Message}");
    }
}
