// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

internal sealed class EpDownloadResultTests
{
    private const string Cuda = "CUDAExecutionProvider";
    private const string WebGpu = "WebGpuExecutionProvider";

    [Test]
    public async Task ExplicitKnownFailure_IsReported()
    {
        var result = Build(
            [Ep(Cuda, false)],
            [Ep(Cuda, false)],
            [Cuda]);

        await Assert.That(result.Success).IsFalse();
        await Assert.That(result.Status).IsEqualTo("Some EPs failed");
        await Assert.That(result.FailedEps).IsEquivalentTo([Cuda]);
        await Assert.That(result.RegisteredEps).IsEmpty();
    }

    [Test]
    public async Task AllProvidersPartialFailure_IsReported()
    {
        var result = Build(
            [Ep(Cuda, false), Ep(WebGpu, false)],
            [Ep(Cuda, true), Ep(WebGpu, false)],
            null);

        await Assert.That(result.Success).IsFalse();
        await Assert.That(result.Status).IsEqualTo("Some EPs failed");
        await Assert.That(result.RegisteredEps).IsEquivalentTo([Cuda]);
        await Assert.That(result.FailedEps).IsEquivalentTo([WebGpu]);
    }

    [Test]
    public async Task ExplicitUnknownProvider_IsReported()
    {
        var result = Build([], [], ["UnknownExecutionProvider"]);

        await Assert.That(result.Success).IsFalse();
        await Assert.That(result.Status).IsEqualTo("Some EPs failed");
        await Assert.That(result.FailedEps).IsEquivalentTo(["UnknownExecutionProvider"]);
    }

    [Test]
    public async Task AlreadyRegisteredProvider_SucceedsWithoutRegistrationDelta()
    {
        var result = Build(
            [Ep(Cuda, true)],
            [Ep(Cuda, true)],
            [Cuda]);

        await Assert.That(result.Success).IsTrue();
        await Assert.That(result.Status).IsEqualTo("All EPs registered");
        await Assert.That(result.RegisteredEps).IsEmpty();
        await Assert.That(result.FailedEps).IsEmpty();
    }

    [Test]
    public async Task NewlyRegisteredProvider_IsReported()
    {
        var result = Build(
            [Ep(Cuda, false)],
            [Ep(Cuda, true)],
            [Cuda]);

        await Assert.That(result.Success).IsTrue();
        await Assert.That(result.Status).IsEqualTo("All EPs registered");
        await Assert.That(result.RegisteredEps).IsEquivalentTo([Cuda]);
        await Assert.That(result.FailedEps).IsEmpty();
    }

    private static EpDownloadResult Build(
        IReadOnlyCollection<EpInfo> before,
        IReadOnlyCollection<EpInfo> after,
        IReadOnlyCollection<string>? requested) =>
        FoundryLocalManager.BuildEpDownloadResult(before, after, requested);

    private static EpInfo Ep(string name, bool isRegistered) =>
        new() { Name = name, IsRegistered = isRegistered };
}
