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

    [Test]
    public async Task SelectVariant_RefreshesReportedMetadata()
    {
        var catalog = await FoundryLocalManager.Instance.GetCatalogAsync();

        var models = await catalog.ListModelsAsync();
        var modelWithVariants = models.FirstOrDefault(m => m.Variants.Count > 1);

        if (modelWithVariants == null)
        {
            Skip.Test("No multi-variant model in the catalog.");
            return;
        }

        var variants = modelWithVariants.Variants.ToList();

        // Derive the original variant from the currently-selected Info rather than
        // assuming variants[0]: native selection prefers the first cached variant.
        var infoBefore = modelWithVariants.Info;
        var defaultVariant = variants.First(v => v.Id == infoBefore.Id);
        var otherVariant = variants.First(v => v.Id != infoBefore.Id);

        modelWithVariants.SelectVariant(otherVariant);

        // Native is the source of truth: every read must reflect the selected variant.
        await Assert.That(modelWithVariants.Id).IsEqualTo(otherVariant.Id);
        await Assert.That(modelWithVariants.Alias).IsEqualTo(otherVariant.Alias);

        var infoAfter = modelWithVariants.Info;
        await Assert.That(infoAfter.Id).IsEqualTo(otherVariant.Id);
        await Assert.That(infoAfter.Name).IsEqualTo(otherVariant.Info.Name);
        await Assert.That(infoAfter.Version).IsEqualTo(otherVariant.Info.Version);

        // The earlier snapshot is an independent point-in-time value.
        await Assert.That(infoBefore.Id).IsEqualTo(defaultVariant.Id);

        // Selecting back refreshes again.
        modelWithVariants.SelectVariant(defaultVariant);
        await Assert.That(modelWithVariants.Info.Id).IsEqualTo(defaultVariant.Id);
    }
}
