// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;

[SkipUnlessIntegration]
internal sealed class EndToEnd
{
    // end-to-end using real catalog. run manually as a standalone test as it alters the model cache.
    [Test]
    public async Task EndToEndTest_Succeeds()
    {
        var manager = FoundryLocalManager.Instance; // initialized by Utils
        var catalog = await manager.GetCatalogAsync();

        var models = await catalog.ListModelsAsync().ConfigureAwait(false);

        await Assert.That(models).IsNotNull();
        await Assert.That(models.Count).IsGreaterThan(0);

        // Use a dedicated model alias so selecting this variant does not affect the
        // catalog tests when TUnit runs them in parallel.
        var modelVariant = await catalog.GetModelVariantAsync("qwen2.5-0.5b-instruct-generic-cpu:4")
                                        .ConfigureAwait(false);

        await Assert.That(modelVariant).IsNotNull();
        await Assert.That(modelVariant!.Alias).IsEqualTo("qwen2.5-0.5b");

        // Get Model for variant and select the variant so `model` and `modelVariant` should be equivalent
        var model = await catalog.GetModelAsync(modelVariant.Alias);
        model!.SelectVariant(modelVariant);

        // uncomment this to remove the model first to test the download progress
        // only do this when manually testing as other tests expect the model to be cached
        //await modelVariant.RemoveFromCacheAsync().ConfigureAwait(false);
        //await Assert.That(modelVariant.IsCached).IsFalse(); // check variant status matches

        var expectedCallbackUsed = !await modelVariant.IsCachedAsync();
        var progressValues = new List<float>();
        var addProgressCallbackValue = new Action<float>(progressValues.Add);

        await model.DownloadAsync(addProgressCallbackValue);

        if (expectedCallbackUsed)
        {
            await Assert.That(progressValues).IsNotEmpty();
            await Assert.That(progressValues[^1]).IsEqualTo(100.0f);
        }
        else
        {
            await Assert.That(progressValues[0]).IsEqualTo(100.0f);
        }

        await Assert.That(await modelVariant.IsCachedAsync()).IsTrue();  // check variant status matches

        var path = await modelVariant.GetPathAsync().ConfigureAwait(false);
        var modelPath = await model.GetPathAsync().ConfigureAwait(false);
        await Assert.That(path).IsNotNull();
        await Assert.That(modelPath).IsEqualTo(path);

        await modelVariant.LoadAsync().ConfigureAwait(false);
        await Assert.That(await modelVariant.IsLoadedAsync()).IsTrue();
        await Assert.That(await model.IsLoadedAsync()).IsTrue();

        // check we get the same info from the web service
        await manager.StartWebServiceAsync();
        try
        {
            await Assert.That(manager.Urls).IsNotNull();
            var serviceUri = new Uri(manager.Urls![0]);

            // create model load manager that queries the web service
            var loadedModels = await catalog.GetLoadedModelsAsync().ConfigureAwait(false);
            await Assert.That(loadedModels.Any(m => m.Id == modelVariant!.Id)).IsTrue();
        }
        finally
        {
            await manager.StopWebServiceAsync().ConfigureAwait(false);
        }

        // Unload happens in TestAssemblySetupCleanup so tests don't affect each other.
        //await modelVariant.UnloadAsync().ConfigureAwait(false);
        //await Assert.That(modelVariant.IsLoaded).IsFalse();
    }
}
