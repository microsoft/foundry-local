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
