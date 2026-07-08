// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

[SkipUnlessIntegration]
internal sealed class CatalogTests
{
    [Test]
    public async Task GetLatestVersion_Works()
    {
        // Use the real catalog from the initialized FoundryLocalManager
        var catalog = await FoundryLocalManager.Instance.GetCatalogAsync();

        // Get all models and find one with multiple variants to test version sorting
        var models = await catalog.ListModelsAsync();
        await Assert.That(models).IsNotNull().And.IsNotEmpty();

        // Find a model with variants to test GetLatestVersionAsync
        var modelWithVariants = models.FirstOrDefault(m => m.Variants.Count > 1);

        if (modelWithVariants == null)
        {
            // If no model has multiple variants, just verify GetLatestVersion returns the same model
            var singleModel = models.First();
            var result = await catalog.GetLatestVersionAsync(singleModel);
            await Assert.That(result).IsNotNull();
            await Assert.That(result.Id).IsEqualTo(singleModel.Id);
            return;
        }

        // Get the variants
        var variants = modelWithVariants.Variants.ToList();
        await Assert.That(variants.Count).IsGreaterThanOrEqualTo(2);

        // GetLatestVersion for any variant should return the first variant (highest version)
        var latestVariant = variants[0];
        var otherVariant = variants[^1]; // last variant (oldest version)

        var result1 = await catalog.GetLatestVersionAsync(latestVariant);
        await Assert.That(result1.Id).IsEqualTo(latestVariant.Id);

        var result2 = await catalog.GetLatestVersionAsync(otherVariant);
        await Assert.That(result2.Id).IsEqualTo(latestVariant.Id);

        // Test with Model input — when latest is selected, should get matching model back
        modelWithVariants.SelectVariant(latestVariant);
        var result3 = await catalog.GetLatestVersionAsync(modelWithVariants);
        await Assert.That(result3.Id).IsEqualTo(modelWithVariants.Id);
    }

    [Test]
    public async Task ListModelsAsync_HonorsCancelledToken()
    {
        var catalog = await FoundryLocalManager.Instance.GetCatalogAsync();

        using var cts = new CancellationTokenSource();
        cts.Cancel();

        await Assert.That(async () => await catalog.ListModelsAsync(cts.Token).ConfigureAwait(false))
            .Throws<OperationCanceledException>();
    }
}
