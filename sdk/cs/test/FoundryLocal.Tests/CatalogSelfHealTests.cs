// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.Extensions.Logging.Abstractions;

using Moq;

/// <summary>
/// Catalog self-heal behavior — when a public lookup (<c>GetModelAsync</c>,
/// <c>GetModelVariantAsync</c>, <c>GetCachedModelsAsync</c>) misses against
/// the in-memory cache, the Catalog issues one forced refresh
/// (<c>UpdateModels(force: true)</c>) bypassing the 6h TTL gate and retries.
/// This surfaces BYOM (Bring-Your-Own-Model) entries that were dropped into
/// the cache directory after the SDK warmed up. Empty / whitespace input must
/// short-circuit without firing the (expensive) forced refresh.
/// </summary>
internal sealed class CatalogSelfHealTests
{
    internal sealed class CallCounters
    {
        public int ModelListCalls;
    }

    private static ModelInfo MakeModelInfo(string id, string alias, bool cached)
    {
        var version = int.Parse(id.Split(':').Last());
        return new ModelInfo
        {
            Id = id,
            Name = alias,
            Version = version,
            Alias = alias,
            DisplayName = alias,
            ProviderType = "local",
            Uri = $"local://{alias}/{version}",
            ModelType = "ONNX",
            Runtime = new Runtime { DeviceType = DeviceType.CPU, ExecutionProvider = "CPUExecutionProvider" },
            Cached = cached,
        };
    }

    /// <summary>
    /// Build a Catalog whose <c>get_model_list</c> IPC returns
    /// <paramref name="modelListResponses"/>[N] on the (N+1)-th call (clamping
    /// to the last entry) and whose <c>get_cached_models</c> IPC returns
    /// <paramref name="cachedModelIds"/>. The shared <see cref="CallCounters"/>
    /// exposes the model-list IPC call count for the test to assert against.
    /// </summary>
    private static async Task<(Catalog Catalog, CallCounters Counters)> CreateCatalogAsync(
        IReadOnlyList<IReadOnlyList<ModelInfo>> modelListResponses,
        string[]? cachedModelIds = null)
    {
        var counters = new CallCounters();
        var mockCoreInterop = new Mock<ICoreInterop>();

        mockCoreInterop.Setup(x => x.ExecuteCommand("get_catalog_name", It.IsAny<CoreInteropRequest?>()))
            .Returns(new ICoreInterop.Response { Data = "TestCatalog", Error = null });

        mockCoreInterop.Setup(x => x.ExecuteCommandAsync("get_model_list", It.IsAny<CoreInteropRequest?>(),
                                                         It.IsAny<CancellationToken?>()))
            .ReturnsAsync(() =>
            {
                var idx = Math.Min(counters.ModelListCalls, modelListResponses.Count - 1);
                counters.ModelListCalls++;
                var data = JsonSerializer.Serialize(
                    modelListResponses[idx].ToList(), JsonSerializationContext.Default.ListModelInfo);
                return new ICoreInterop.Response { Data = data, Error = null };
            });

        var cachedJson = JsonSerializer.Serialize(cachedModelIds ?? []);
        mockCoreInterop.Setup(x => x.ExecuteCommandAsync("get_cached_models", It.IsAny<CoreInteropRequest?>(),
                                                         It.IsAny<CancellationToken?>()))
            .ReturnsAsync(new ICoreInterop.Response { Data = cachedJson, Error = null });

        var mockLoadManager = new Mock<IModelLoadManager>();
        mockLoadManager.Setup(x => x.ListLoadedModelsAsync(It.IsAny<CancellationToken?>()))
            .ReturnsAsync([]);

        var catalog = await Catalog.CreateAsync(mockLoadManager.Object, mockCoreInterop.Object,
                                                NullLogger<Catalog>.Instance, null);
        return (catalog, counters);
    }

    [Test]
    public async Task SelfHealsGetModelAndGetModelVariantOnCacheMiss()
    {
        // Warm catalog returns no models; the forced self-heal refresh from
        // the public miss path returns the BYOM that was dropped into the
        // cache after the SDK warmed up. CreateAsync auto-warms (1 call), the
        // first GetModelAsync miss forces the second call (the self-heal),
        // and the subsequent GetModelVariantAsync hits the now-populated map
        // directly — the inner TTL-gated UpdateModels short-circuits.
        ModelInfo[] byom = [MakeModelInfo("byom-self-heal:1", "byom-self-heal", cached: true)];
        var (catalog, counters) = await CreateCatalogAsync(
        [
            [],
            byom,
        ]);

        var model = await catalog.GetModelAsync("byom-self-heal");
        await Assert.That(model).IsNotNull();
        await Assert.That(model!.Alias).IsEqualTo("byom-self-heal");

        var variant = await catalog.GetModelVariantAsync("byom-self-heal:1");
        await Assert.That(variant).IsNotNull();
        await Assert.That(variant!.Id).IsEqualTo("byom-self-heal:1");

        await Assert.That(counters.ModelListCalls).IsEqualTo(2);
    }

    [Test]
    public async Task SelfHealsGetCachedModelsOnUnknownId()
    {
        // Core always reports the BYOM as cached; the SDK has to self-heal in
        // ResolveModelIdsAsync (force-refresh) to learn the id exists in the
        // catalog and surface it from GetCachedModelsAsync. CreateAsync warms
        // with an empty model list (1 call); GetCachedModelsAsync sees the
        // unknown id and forces a second refresh (now byom).
        ModelInfo[] byom = [MakeModelInfo("byom-cached:1", "byom-cached", cached: true)];
        var (catalog, counters) = await CreateCatalogAsync(
            [[], byom],
            cachedModelIds: ["byom-cached:1"]);

        var cached = await catalog.GetCachedModelsAsync();

        await Assert.That(cached.Count).IsEqualTo(1);
        await Assert.That(cached[0].Id).IsEqualTo("byom-cached:1");
        await Assert.That(counters.ModelListCalls).IsEqualTo(2);
    }

    [Test]
    public async Task DoesNotRefreshCatalogOnEmptyOrWhitespaceInput()
    {
        // Empty / whitespace / null input to GetModelAsync / GetModelVariantAsync
        // must short-circuit and return null WITHOUT firing the expensive
        // forced refresh that powers the self-heal path. Without the guard, a
        // tight loop that sometimes passes "" would trigger a recursive disk
        // scan in Core on every call.
        var (catalog, counters) = await CreateCatalogAsync([[]]);

        // After CreateAsync there is exactly one model-list IPC (the warm).
        await Assert.That(counters.ModelListCalls).IsEqualTo(1);

        foreach (var invalid in new[] { string.Empty, "   ", null })
        {
            await Assert.That(await catalog.GetModelAsync(invalid!)).IsNull();
            await Assert.That(await catalog.GetModelVariantAsync(invalid!)).IsNull();
        }

        await Assert.That(counters.ModelListCalls).IsEqualTo(1);
    }
}
