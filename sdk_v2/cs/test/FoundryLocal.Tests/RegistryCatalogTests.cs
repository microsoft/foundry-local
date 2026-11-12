// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Serialization;

using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.AI.Foundry.Local.Detail.Native;
using Microsoft.Extensions.Logging.Abstractions;

using NativeManager = Microsoft.AI.Foundry.Local.Detail.Native.Manager;
using PublicConfiguration = Microsoft.AI.Foundry.Local.Configuration;

#pragma warning disable TUnitAssertions0005

[NotInParallel]
internal sealed class RegistryCatalogTests
{
    [Test]
    [Arguments(4u)]
    [Arguments(3u)]
    [Arguments(2u)]
    [Arguments(0u)]
    public async Task Negotiation_TriesEverySupportedVersionInDescendingOrder(uint supportedVersion)
    {
        var attempted = new List<uint>();
        var ptr = Api.NegotiateApi(version =>
        {
            attempted.Add(version);
            return version == supportedVersion ? new IntPtr(42) : IntPtr.Zero;
        }, out var selected);

        await Assert.That(attempted).IsEquivalentTo(
            supportedVersion == 4 ? new uint[] { 4 } :
            supportedVersion == 3 ? new uint[] { 4, 3 } : new uint[] { 4, 3, 2 });
        await Assert.That(attempted.SequenceEqual(attempted.OrderByDescending(v => v))).IsTrue();
        await Assert.That(ptr).IsEqualTo(supportedVersion == 0 ? IntPtr.Zero : new IntPtr(42));
        if (supportedVersion != 0)
        {
            await Assert.That(selected).IsEqualTo(supportedVersion);
        }
    }

    [Test]
    public async Task V4Slot_IsAppendedAfterUnchangedV2AndV3RootTable()
    {
        var size = Marshal.SizeOf<FlApi>();
        var table = Marshal.AllocHGlobal(size + IntPtr.Size);
        FlApi_ManagerGetCatalogWithOptionsDelegate getter =
            (IntPtr manager, FlCatalogType type, IntPtr options, out IntPtr catalog) =>
            {
                catalog = new IntPtr(37);
                return IntPtr.Zero;
            };
        try
        {
            await Assert.That(Marshal.OffsetOf<FlApi>(nameof(FlApi.ManagerGetCatalogByType)).ToInt64())
                .IsEqualTo(29L * IntPtr.Size);
            await Assert.That(size).IsEqualTo(30 * IntPtr.Size);
            Marshal.WriteIntPtr(table, size, Marshal.GetFunctionPointerForDelegate(getter));
            var read = Api.ReadCatalogWithOptions(table, 4)!;
            read(IntPtr.Zero, FlCatalogType.AzureRegistry, IntPtr.Zero, out var catalog);
            await Assert.That(catalog).IsEqualTo(new IntPtr(37));
            Marshal.WriteIntPtr(table, size, IntPtr.Zero);
            await Assert.That(() => Api.ReadCatalogWithOptions(table, 4)).Throws<InvalidOperationException>();
        }
        finally
        {
            Marshal.FreeHGlobal(table);
            GC.KeepAlive(getter);
        }
    }

    [Test]
    [Arguments(2u)]
    [Arguments(3u)]
    public async Task OlderTables_DoNotReadAppendedV4Slot(uint version)
    {
        await Assert.That(Api.ReadCatalogWithOptions(IntPtr.Zero, version)).IsNull();
    }

    [Test]
    public async Task LegacyRoutes_UseByTypeAndReuseWrappersWithoutV4()
    {
        using var mocks = new NativeMocks();
        Api.GetCatalogWithOptions = null;
        var defaultCatalog = await mocks.Manager.GetCatalogAsync();
        var publicCatalog = await mocks.Manager.GetCatalogAsync(CatalogType.Public);
        var emptyOptions = await mocks.Manager.GetCatalogAsync(CatalogType.Public, new Dictionary<string, string>());
        var localCatalog = await mocks.Manager.GetCatalogAsync(CatalogType.Local);

        await Assert.That(ReferenceEquals(defaultCatalog, publicCatalog)).IsTrue();
        await Assert.That(ReferenceEquals(defaultCatalog, emptyOptions)).IsTrue();
        await Assert.That(ReferenceEquals(defaultCatalog, localCatalog)).IsFalse();
        await Assert.That(mocks.TypedCalls).IsEquivalentTo(
            new[] { FlCatalogType.Public, FlCatalogType.Public, FlCatalogType.Public, FlCatalogType.Local });
        await Assert.That(mocks.OptionsCalls).IsEqualTo(0);
    }

    [Test]
    public async Task RegistryOptions_AreSnapshottedForwardedAndCachedByNativePointer()
    {
        using var mocks = new NativeMocks();
        var options = new Dictionary<string, string> { ["name"] = "first", ["credential"] = "azure_cli" };
        var pending = mocks.Manager.GetCatalogAsync(CatalogType.AzureRegistry, options);
        options["name"] = "second";
        var first = await pending;
        await Assert.That(mocks.Settings["name"]).IsEqualTo("first");
        await Assert.That(mocks.Settings["credential"]).IsEqualTo("azure_cli");
        await Assert.That(mocks.LastType).IsEqualTo(FlCatalogType.AzureRegistry);

        var second = await mocks.Manager.GetCatalogAsync(CatalogType.AzureRegistry, options);
        options["name"] = "first";
        options["credential"] = "workload_identity";
        var sameNativeCatalog = await mocks.Manager.GetCatalogAsync(CatalogType.AzureRegistry, options);

        await Assert.That(ReferenceEquals(first, second)).IsFalse();
        await Assert.That(ReferenceEquals(first, sameNativeCatalog)).IsTrue();
        await Assert.That(mocks.OptionsCalls).IsEqualTo(3);
        await Assert.That(mocks.Releases).IsEqualTo(3);
        await Assert.That(mocks.TypedCalls).IsEmpty();
    }

    [Test]
    public async Task RegistryWithoutOptions_StillUsesV4()
    {
        using var mocks = new NativeMocks();
        await mocks.Manager.GetCatalogAsync(CatalogType.AzureRegistry);
        await Assert.That(mocks.OptionsCalls).IsEqualTo(1);
        await Assert.That(mocks.Settings).IsEmpty();
        await Assert.That(mocks.Releases).IsEqualTo(1);
    }

    [Test]
    [Arguments(CatalogType.AzureRegistry)]
    [Arguments(CatalogType.Public)]
    public async Task OptionsOnOlderRuntimes_FailBeforeAllocation(CatalogType type)
    {
        using var mocks = new NativeMocks();
        Api.GetCatalogWithOptions = null;
        var error = await Assert.ThrowsAsync<FoundryLocalException>(() =>
            mocks.Manager.GetCatalogAsync(type, new Dictionary<string, string> { ["name"] = "first" }));
        await Assert.That(error!.Message).Contains("version 4");
        await Assert.That(mocks.Allocations).IsEqualTo(0);
        await Assert.That(mocks.TypedCalls).IsEmpty();
    }

    [Test]
    public async Task NativeError_ReleasesOptionsAndPreservesErrorCode()
    {
        using var mocks = new NativeMocks();
        Api.GetCatalogWithOptions = (IntPtr manager, FlCatalogType type, IntPtr options, out IntPtr catalog) =>
        {
            catalog = IntPtr.Zero;
            return new IntPtr(99);
        };
        var error = await Assert.ThrowsAsync<FoundryLocalException>(() =>
            mocks.Manager.GetCatalogAsync(CatalogType.AzureRegistry));
        await Assert.That(error!.ErrorCode).IsEqualTo(FoundryLocalErrorCode.Network);
        await Assert.That(mocks.Releases).IsEqualTo(1);
        await Assert.That(mocks.StatusReleases).IsEqualTo(1);
    }

    [Test]
    public async Task NullCatalog_IsRejectedAndOptionsAreReleased()
    {
        using var mocks = new NativeMocks();
        Api.GetCatalogWithOptions = (IntPtr manager, FlCatalogType type, IntPtr options, out IntPtr catalog) =>
        {
            catalog = IntPtr.Zero;
            return IntPtr.Zero;
        };
        var error = await Assert.ThrowsAsync<FoundryLocalException>(() =>
            mocks.Manager.GetCatalogAsync(CatalogType.AzureRegistry));
        await Assert.That(error!.Message).Contains("no AzureRegistry catalog");
        await Assert.That(mocks.Releases).IsEqualTo(1);
    }

    [Test]
    [Arguments("", "value")]
    [Arguments("bad\0key", "value")]
    [Arguments("name", "bad\0value")]
    [Arguments("name", null)]
    public async Task InvalidOptions_AreRejectedBeforeNativeDispatch(string key, string? value)
    {
        using var mocks = new NativeMocks();
        await Assert.ThrowsAsync<ArgumentException>(() => mocks.Manager.GetCatalogAsync(
            CatalogType.AzureRegistry, new Dictionary<string, string> { [key] = value! }));
        await Assert.That(mocks.Allocations).IsEqualTo(0);
    }

    [Test]
    public async Task CanceledAndDisposedManagers_DoNotDispatch()
    {
        using var mocks = new NativeMocks();
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        await Assert.ThrowsAsync<OperationCanceledException>(() =>
            mocks.Manager.GetCatalogAsync(CatalogType.AzureRegistry, cancellation.Token));
        mocks.Manager.Dispose();
        await Assert.ThrowsAsync<FoundryLocalException>(() =>
            mocks.Manager.GetCatalogAsync(CatalogType.AzureRegistry));
        await Assert.That(mocks.OptionsCalls).IsEqualTo(0);
    }

    // Registry HTTP and credentials live inside the native runtime. These mocks cover
    // the binding boundary only; shared C++ tests cover the mocked HTTP transport.
    private sealed class NativeMocks : IDisposable
    {
        private readonly FlApi _root = Api.Root;
        private readonly FlCatalogApi _catalog = Api.Catalog;
        private readonly FlApi_ManagerGetCatalogWithOptionsDelegate? _getter = Api.GetCatalogWithOptions;
        private readonly IntPtr _name = Marshal.StringToHGlobalAnsi("mock");
        internal FoundryLocalManager Manager { get; }
        internal Dictionary<string, string> Settings { get; } = new();
        internal List<FlCatalogType> TypedCalls { get; } = new();
        internal FlCatalogType LastType { get; private set; }
        internal int OptionsCalls { get; private set; }
        internal int Allocations { get; private set; }
        internal int Releases { get; private set; }
        internal int StatusReleases { get; private set; }

        internal NativeMocks()
        {
            Api.Root = new FlApi
            {
                CreateKeyValuePairs = (out IntPtr pairs) =>
                {
                    Settings.Clear();
                    Allocations++;
                    pairs = new IntPtr(77);
                },
                AddKeyValuePair = (_, key, value) => Settings.Add(key, value),
                KeyValuePairsRelease = _ => Releases++,
                ManagerGetCatalogByType = (IntPtr manager, FlCatalogType type, out IntPtr catalog) =>
                {
                    TypedCalls.Add(type);
                    catalog = new IntPtr((int)type + 1);
                    return IntPtr.Zero;
                },
                ManagerShutdown = _ => IntPtr.Zero,
                StatusGetErrorCode = _ => FlErrorCode.Network,
                StatusGetErrorMessage = _ => _name,
                StatusRelease = _ => StatusReleases++,
            };
            Api.Catalog = new FlCatalogApi
            {
                GetName = (IntPtr catalog, out IntPtr name) =>
                {
                    name = _name;
                    return IntPtr.Zero;
                },
            };
            Api.GetCatalogWithOptions = (IntPtr manager, FlCatalogType type, IntPtr options, out IntPtr catalog) =>
            {
                LastType = type;
                OptionsCalls++;
                catalog = new IntPtr(Settings.TryGetValue("name", out var name) && name == "second" ? 22 : 11);
                return IntPtr.Zero;
            };

            Manager = (FoundryLocalManager)typeof(FoundryLocalManager)
                .GetConstructor(BindingFlags.Instance | BindingFlags.NonPublic, null,
                    new[] { typeof(PublicConfiguration), typeof(Microsoft.Extensions.Logging.ILogger) }, null)!
                .Invoke(new object[] { new PublicConfiguration { AppName = "registry-mock" }, NullLogger.Instance });
#pragma warning disable SYSLIB0050 // Construct a handle-free native wrapper without loading a runtime.
            var native = (NativeManager)FormatterServices.GetUninitializedObject(typeof(NativeManager));
#pragma warning restore SYSLIB0050
            typeof(FoundryLocalManager).GetField("_nativeManager", BindingFlags.Instance | BindingFlags.NonPublic)!
                .SetValue(Manager, native);
        }

        public void Dispose()
        {
            Manager.Dispose();
            Api.Root = _root;
            Api.Catalog = _catalog;
            Api.GetCatalogWithOptions = _getter;
            Marshal.FreeHGlobal(_name);
        }
    }
}

#pragma warning restore TUnitAssertions0005
