// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

[NotInParallel]
[SkipUnlessIntegration]
internal sealed class ModelManagerLifetimeTests
{
    [Test]
    public async Task ModelOperation_HoldsManagerLeaseAndMetadataSurvivesRecreation()
    {
        var originalManager = FoundryLocalManager.Instance;
        var originalConfiguration = originalManager.Configuration;
        var logger = originalManager.Logger;
        var root = Path.Combine(Path.GetTempPath(), $"foundry-local-cs-model-lifetime-{Guid.NewGuid():N}");
        var modelPath = Path.Combine(root, "model");
        var modelId = $"cs-model-lifetime-{Guid.NewGuid():N}:1";
        Directory.CreateDirectory(modelPath);
        File.WriteAllText(Path.Combine(modelPath, "genai_config.json"),
                          "{\"model\":{\"type\":\"phi3\",\"context_length\":4096}}");

        try
        {
            var catalog = await originalManager.GetCatalogAsync(CatalogType.Local);
            var metadata = new ModelInfoBuilder()
                .SetStringProperty(ModelInfoPropertyKeys.Task, "chat-completion")
                .SetStringProperty("custom_metadata", "recreated")
                .SetIntProperty("custom_count", 73);
            var model = (Model)await catalog.RegisterModelAsync(modelPath, modelId, metadata);

            model.BeforeNativeCallForTest = originalManager.Dispose;
            await Assert.That(() => model.GetStringProperty("custom_metadata")).Throws<InvalidOperationException>();
            await Assert.That(FoundryLocalManager.Instance).IsSameReferenceAs(originalManager);
            model.BeforeNativeCallForTest = null;

            using var operationEntered = new ManualResetEventSlim(false);
            using var disposeStarted = new ManualResetEventSlim(false);
            model.BeforeNativeCallForTest = () =>
            {
                operationEntered.Set();
                disposeStarted.Wait();
            };
            originalManager.BeforeNativeDisposeForTest = disposeStarted.Set;

            var readTask = Task.Run(() => model.GetStringProperty("custom_metadata"));
            operationEntered.Wait();
            var disposeTask = Task.Run(originalManager.Dispose);

            await Assert.That(await readTask).IsEqualTo("recreated");
            await disposeTask;
            await Assert.That(() => model.Info).Throws<ObjectDisposedException>();

            await FoundryLocalManager.CreateAsync(originalConfiguration, logger);
            var recreatedCatalog = await FoundryLocalManager.Instance.GetCatalogAsync(CatalogType.Local);
            var recreated = await recreatedCatalog.GetModelVariantAsync(modelId);
            await Assert.That(recreated).IsNotNull();
            await Assert.That(recreated!.GetStringProperty("custom_metadata")).IsEqualTo("recreated");
            await Assert.That(recreated.GetIntProperty("custom_count")).IsEqualTo(73);
            await recreatedCatalog.UnregisterModelAsync(modelId);
        }
        finally
        {
            if (!FoundryLocalManager.IsInitialized)
            {
                await FoundryLocalManager.CreateAsync(originalConfiguration, logger);
            }
            Directory.Delete(root, recursive: true);
        }
    }
}