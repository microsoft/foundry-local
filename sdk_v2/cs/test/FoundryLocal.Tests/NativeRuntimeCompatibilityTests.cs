// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.AI.Foundry.Local.Detail.Native;

using PublicConfiguration = Microsoft.AI.Foundry.Local.Configuration;

internal sealed class NativeRuntimeCompatibilityTests
{
    [Test]
    [NotInParallel]
    public async Task WebServiceActivityPreservesManagerOwnershipAndListener()
    {
        var ownsManager = !FoundryLocalManager.IsInitialized;
        if (ownsManager)
        {
            await FoundryLocalManager.CreateAsync(new PublicConfiguration
            {
                AppName = "http-activity-test",
                AppDataDir = Path.Combine(AppContext.BaseDirectory, "TestResults", "http-activity-test"),
                DisableNonessentialTelemetry = true,
                UseSharedHost = true
            }, Microsoft.Extensions.Logging.Abstractions.NullLogger.Instance);
        }

        var manager = FoundryLocalManager.Instance;
        try
        {
            var urls = manager.Urls;
            var first = manager.GetWebServiceActivity();
            await Task.Delay(20);
            var second = manager.GetWebServiceActivity();
            await Assert.That(second.ActiveRequests).IsEqualTo(0UL);
            await Assert.That(second.IdleDuration).IsGreaterThanOrEqualTo(first.IdleDuration);
            await Assert.That(manager.Urls ?? []).IsEquivalentTo(urls ?? []);
            await Assert.That(FoundryLocalManager.Instance).IsSameReferenceAs(manager);
            await Assert.That(() => manager.TryStopWebServiceIfIdle(TimeSpan.FromTicks(-1)))
                .Throws<ArgumentOutOfRangeException>();
            await Assert.That(manager.TryStopWebServiceIfIdle(TimeSpan.MaxValue)).IsFalse();
            if (ownsManager)
            {
                manager.Dispose();
                await Assert.That(() => manager.GetWebServiceActivity()).Throws<ObjectDisposedException>();
                await Assert.That(() => manager.TryStopWebServiceIfIdle(TimeSpan.Zero)).Throws<ObjectDisposedException>();
            }
        }
        finally
        {
            // Integration runs borrow the assembly fixture's manager.
            if (ownsManager)
            {
                manager.Dispose();
            }
        }
    }

    [Test]
    [NotInParallel]
    [SkipUnlessIntegration]
    public async Task UrlsObserveNativeListenerChanges()
    {
        var manager = FoundryLocalManager.Instance;
        var wasRunning = manager.Urls is not null;
        try
        {
            manager.NativeManager.StopService();
            await Assert.That(manager.Urls).IsNull();
            manager.NativeManager.StartService();
            await Assert.That(manager.Urls).IsNotNull();
            await Assert.That(manager.Urls!.Length).IsGreaterThan(0);
            manager.NativeManager.StopService();
            await Assert.That(manager.Urls).IsNull();
        }
        finally
        {
            if (wasRunning)
            {
                manager.NativeManager.StartService();
            }
            else
            {
                manager.NativeManager.StopService();
            }
        }
    }

    [Test]
    public async Task IncompatibleRuntimeMessageIncludesLoadedAndRequiredVersions()
    {
        const string loadedRuntimeVersion = "0.5.0-test";

        var message = Api.CreateIncompatibleRuntimeMessage(loadedRuntimeVersion);

        await Assert.That(message).IsEqualTo(
            $"FoundryLocalGetApi({NativeMethods.ApiVersion}) returned null. The loaded native Foundry Local runtime "
            + "reports product version '0.5.0-test', but this SDK requires C API table version "
            + $"{NativeMethods.ApiVersion}. Update the native runtime ({NativeMethods.LibraryName} and the libraries "
            + "shipped with it).");
    }

    [Test]
    public async Task Version2DoesNotReadTheAppendedConfigurationSetting()
    {
        await Assert.That(Api.ReadSharedHostSetting(IntPtr.Zero, 2)).IsNull();
    }

    [Test]
    public async Task OlderRuntimesDoNotReadTheAppendedActivityMethod()
    {
        await Assert.That(Api.ReadWebServiceActivity(IntPtr.Zero, 2)).IsNull();
        await Assert.That(Api.ReadWebServiceActivity(IntPtr.Zero, 3)).IsNull();
    }

    [Test]
    public async Task OlderRuntimesDoNotReadTheAppendedIdleStopMethod()
    {
        await Assert.That(Api.ReadTryStopWebServiceIfIdle(IntPtr.Zero, 2)).IsNull();
        await Assert.That(Api.ReadTryStopWebServiceIfIdle(IntPtr.Zero, 3)).IsNull();
    }

    [Test]
    public async Task IdleStopRejectsNegativeDurationsBeforeCallingNative()
    {
        await Assert.That(() => Detail.Native.Manager.ValidateMinimumIdleDuration(TimeSpan.FromTicks(-1)))
            .Throws<ArgumentOutOfRangeException>();
        await Assert.That(Detail.Native.Manager.ValidateMinimumIdleDuration(TimeSpan.Zero)).IsEqualTo(0UL);
        await Assert.That(Detail.Native.Manager.ValidateMinimumIdleDuration(TimeSpan.FromTicks(1))).IsEqualTo(1UL);
        await Assert.That(Detail.Native.Manager.ValidateMinimumIdleDuration(TimeSpan.MaxValue))
            .IsEqualTo(922337203685478UL);
    }

    [Test]
    [NotInParallel]
    public async Task IdleStopUsesTheAppendedNativeMethodAndRejectsAnOlderRuntime()
    {
        using var configuration = new Detail.Native.Configuration("idle-stop-native-compatibility");
        configuration.SetAppDataDir(Path.Combine(AppContext.BaseDirectory, "TestResults", "idle-stop-native-compatibility"));
        using var manager = new Detail.Native.Manager(configuration);
        var stop = Api.TryStopWebServiceIfIdle;
        try
        {
            Api.TryStopWebServiceIfIdle = null;
            await Assert.That(() => manager.TryStopWebServiceIfIdle(TimeSpan.Zero)).Throws<NotSupportedException>();
            await Assert.That(() => manager.TryStopWebServiceIfIdle(TimeSpan.FromTicks(-1)))
                .Throws<ArgumentOutOfRangeException>();
        }
        finally
        {
            Api.TryStopWebServiceIfIdle = stop;
        }

        manager.StartService();
        await Assert.That(manager.TryStopWebServiceIfIdle(TimeSpan.MaxValue)).IsFalse();
        await Assert.That(manager.GetServiceUrls().Length).IsGreaterThan(0);
        await Assert.That(manager.TryStopWebServiceIfIdle(TimeSpan.Zero)).IsTrue();
        await Assert.That(manager.GetServiceUrls().Length).IsEqualTo(0);
        manager.StartService();
        await Assert.That(manager.GetServiceUrls().Length).IsGreaterThan(0);
        manager.StopService();
    }

    [Test]
    public async Task SharedHostConfigurationRejectsExternalServiceBeforeInitialization()
    {
        var configuration = new PublicConfiguration
        {
            AppName = "hosted-config-test",
            UseSharedHost = true,
            Web = new PublicConfiguration.WebService { ExternalUrl = new Uri("http://127.0.0.1:1234") }
        };
        await Assert.That(() => configuration.Validate()).Throws<ArgumentException>();
    }

    [Test]
    public async Task Version3MarshalsSharedHostingToTheNativeConfiguration()
    {
        using var configuration = new Detail.Native.Configuration("hosted-abi-test");
        configuration.SetUseSharedHost(true).SetExternalServiceUrl("http://127.0.0.1:1234");
        try
        {
            using var manager = new Detail.Native.Manager(configuration);
            Assert.Fail("Native shared hosting must reject an external service before initialization.");
        }
        catch (FoundryLocalException error)
        {
            await Assert.That(error.ErrorCode).IsEqualTo(FoundryLocalErrorCode.InvalidArgument);
            await Assert.That(error.Message).Contains("shared hosting cannot use an external service");
        }
    }
}
