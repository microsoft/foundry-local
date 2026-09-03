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
        var catalog = await FoundryLocalManager.Instance.GetCatalogAsync();

        // Use a dedicated alias so this test does not change the selected variant of a
        // model used by another parallel test.
        var modelWithVersions = await catalog.GetModelAsync("qwen2.5-coder-0.5b");
        if (modelWithVersions == null)
        {
            Skip.Test("The qwen2.5-coder-0.5b model is not available in the catalog.");
            return;
        }

        var versions = modelWithVersions.Variants
            .GroupBy(v => v.Info.Name)
            .FirstOrDefault(group => group.Count() > 1)?
            .ToList() ?? [];
        if (versions.Count < 2)
        {
            Skip.Test("The qwen2.5-coder-0.5b model has no historical versions in the catalog.");
            return;
        }

        await Assert.That(versions.Count).IsGreaterThanOrEqualTo(2);

        // Variants are sorted by version descending within a device/name group.
        var latestVariant = versions[0];
        var otherVariant = versions[^1];
        var originalVariant = modelWithVersions.Variants.First(v => v.Id == modelWithVersions.Id);

        var result1 = await catalog.GetLatestVersionAsync(latestVariant);
        await Assert.That(result1.Id).IsEqualTo(latestVariant.Id);

        var result2 = await catalog.GetLatestVersionAsync(otherVariant);
        await Assert.That(result2.Id).IsEqualTo(latestVariant.Id);

        try
        {
            // Test with Model input — when latest is selected, should get matching model back.
            modelWithVersions.SelectVariant(latestVariant);
            var result3 = await catalog.GetLatestVersionAsync(modelWithVersions);
            await Assert.That(result3.Id).IsEqualTo(latestVariant.Id);
        }
        finally
        {
            modelWithVersions.SelectVariant(originalVariant);
        }
    }

    [Test]
    public async Task GetModelVersionsAsync_ReturnsVersions_AndCapsResults()
    {
        var catalog = await FoundryLocalManager.Instance.GetCatalogAsync();

        var models = await catalog.ListModelsAsync();
        var modelWithVariants = models.FirstOrDefault(m => m.Variants.Count > 1);

        if (modelWithVariants == null)
        {
            Skip.Test("No multi-version model in the catalog.");
            return;
        }

        var versions = await catalog.GetModelVersionsAsync(modelWithVariants.Alias);
        await Assert.That(versions).IsNotNull();
        await Assert.That(versions.Count).IsGreaterThanOrEqualTo(2);

        var capped = await catalog.GetModelVersionsAsync(modelWithVariants.Alias, maxVersions: 1);

        // maxVersions caps the number of versions returned *per model name*, not the total
        // result count. An alias with several distinct model names can therefore return one
        // entry per name, so assert the cap per name rather than on the overall count.
        var perName = capped.GroupBy(v => v.Info.Name);
        foreach (var group in perName)
        {
            await Assert.That(group.Count()).IsLessThanOrEqualTo(1);
        }
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

        // This alias is intentionally distinct from the aliases used by the latest-version
        // and end-to-end tests so their model selections remain independent in parallel runs.
        var modelWithVariants = await catalog.GetModelAsync("qwen3.5-0.8b");
        if (modelWithVariants == null)
        {
            Skip.Test("The qwen3.5-0.8b model is not available in the catalog.");
            return;
        }

        var variants = modelWithVariants.Variants.ToList();
        if (variants.Count < 2)
        {
            Skip.Test("The qwen3.5-0.8b model has fewer than two variants in the catalog.");
            return;
        }

        // Derive the original variant from the currently-selected Info rather than
        // assuming variants[0]: native selection prefers the first cached variant.
        var infoBefore = modelWithVariants.Info;
        var defaultVariant = variants.First(v => v.Id == infoBefore.Id);
        var otherVariant = variants.First(v => v.Id != infoBefore.Id);

        try
        {
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
        }
        finally
        {
            modelWithVariants.SelectVariant(defaultVariant);
        }

        // Selecting back refreshes again.
        await Assert.That(modelWithVariants.Info.Id).IsEqualTo(defaultVariant.Id);
    }
}
