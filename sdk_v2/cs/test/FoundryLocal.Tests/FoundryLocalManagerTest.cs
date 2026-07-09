// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System;
using System.Collections.Generic;
using System.Linq;

using Microsoft.AI.Foundry.Local;

using TUnit.Core.Exceptions;

[SkipUnlessIntegration]
public class FoundryLocalManagerTests
{
    [Test]
    public async Task Manager_GetCatalog_Succeeds()
    {
        var catalog = await FoundryLocalManager.Instance.GetCatalogAsync() as Catalog;
        await Assert.That(catalog).IsNotNull();
        await Assert.That(catalog!.Name).IsNotNullOrWhitespace();

        var models = await catalog.ListModelsAsync();
        await Assert.That(models).IsNotNull().And.IsNotEmpty();

        foreach (var model in models)
        {
            Console.WriteLine($"Model Alias: {model.Alias}, Variants: {model.Variants.Count}");
            Console.WriteLine($"Selected Variant Id: {model.Id ?? "none"}");

            // variants should be in sorted order

            DeviceType lastDeviceType = DeviceType.Invalid;
            var lastName = string.Empty;
            var lastVersion = int.MaxValue;

            foreach (var variant in model.Variants)
            {
                Console.WriteLine($"  Id: {variant.Id}, Cached={variant.Info.Cached}");

                // variants are grouped by name
                // check if variants are sorted by device type and version
                if ((variant.Info.Name == lastName) &&
                    ((variant.Info.Runtime?.DeviceType > lastDeviceType) ||
                     (variant.Info.Runtime?.DeviceType == lastDeviceType && variant.Info.Version > lastVersion)))
                {
                    Assert.Fail($"Variant {variant.Id} is not in the expected order.");
                }

                lastDeviceType = variant.Info.Runtime?.DeviceType ?? DeviceType.Invalid;
                lastName = variant.Info.Name;
                lastVersion = variant.Info.Version;
            }
        }
    }

    [Test]
    public async Task Catalog_ListCachedLoadUnload_Succeeds()
    {
        var catalog = await FoundryLocalManager.Instance.GetCatalogAsync();
        await Assert.That(catalog).IsNotNull();

        var models = await catalog.ListModelsAsync();
        await Assert.That(models).IsNotNull().And.IsNotEmpty();

        var cachedModels = await catalog.GetCachedModelsAsync();
        await Assert.That(cachedModels).IsNotNull();

        if (cachedModels.Count == 0)
        {
            throw new SkipTestException("No cached models found; skipping get path/load/unload test.");
        }

        // find smallest. pick first if no local models have size info.
        // Restrict to CPUExecutionProvider — other EPs (CUDA, WebGPU, ...) require a
        // bootstrapper to be downloaded/registered first via DownloadAndRegisterEps.
        var loadable = cachedModels.Where(m => m.Info.Runtime?.ExecutionProvider == "CPUExecutionProvider").ToList();
        if (loadable.Count == 0)
        {
            throw new SkipTestException("No cached CPU-EP models; skipping load/unload test.");
        }

        var smallest = loadable.Where(m => m.Info.FileSizeMb > 0).OrderBy(m => m.Info.FileSizeMb).FirstOrDefault();
        var variant = smallest ?? loadable[0];

        Console.WriteLine($"Testing GetPath/Load/Unload with ModelId: {variant.Id}");
        var path = await variant.GetPathAsync();
        Console.WriteLine($"Model path: {path}");
        await variant.LoadAsync();

        // We unload any loaded models during cleanup for all tests
        // await variant.UnloadAsync();
    }

    // The following EP-discovery tests mirror the C++ integration tests
    // (EpDetectionApiTest.GetDiscoverableEps_*). They exercise the native
    // flEpInfo* struct-array ABI boundary so an accidental ABI break (or a
    // marshalling regression that reads garbage on a second call) is caught
    // in CI without requiring any model download or EP registration.

    [Test]
    public async Task Manager_DiscoverEps_NamesAreNonEmpty()
    {
        // The contract allows an empty list (a machine may have no discoverable EPs).
        // The array itself must be non-null, and any returned entry must have a
        // non-empty name.
        var eps = FoundryLocalManager.Instance.DiscoverEps();
        await Assert.That(eps).IsNotNull();

        foreach (var ep in eps)
        {
            await Assert.That(ep.Name).IsNotNullOrWhitespace();
        }
    }

    [Test]
    public async Task Manager_DiscoverEps_IsConsistentAcrossCalls()
    {
        // Two consecutive calls must return the same set of EP names. Order is
        // not guaranteed by the contract, so compare as sets — this still
        // catches a binding that reads garbage on the second invocation.
        var first = FoundryLocalManager.Instance.DiscoverEps();
        var second = FoundryLocalManager.Instance.DiscoverEps();

        await Assert.That(first).IsNotNull();
        await Assert.That(second).IsNotNull();

        var firstNames = new HashSet<string>(first.Select(e => e.Name));
        var secondNames = new HashSet<string>(second.Select(e => e.Name));

        await Assert.That(secondNames.SetEquals(firstNames)).IsTrue();
    }
}

