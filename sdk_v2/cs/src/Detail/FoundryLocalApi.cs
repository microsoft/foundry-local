// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

// High-level managed API mirroring the C++ foundry_local class hierarchy.
// Native handles are internal implementation details; public API returns domain classes.

// Suppress IDisposableAnalyzers warnings for the bindings layer:
// - IDISP015: Item cache pattern is intentional (ModelList holds non-owning Model references)
// - IDISP023: Item.~Item finalizer accesses Api which is a static, always available
#pragma warning disable IDISP015
#pragma warning disable IDISP023

namespace Microsoft.AI.Foundry.Local.Detail.Native;

using System.Runtime.InteropServices;

using Microsoft.AI.Foundry.Local.Detail.Interop;


// ===================================================================
// Internal API bootstrap (hidden from consumers)
// ===================================================================

internal static class Api
{
    internal static FlApi Root;
    internal static FlItemApi Item;
    internal static FlInferenceApi Inference;
    internal static FlConfigurationApi Config;
    internal static FlCatalogApi Catalog;
    internal static FlModelApi Model;
    private static volatile bool _initialized;
    private static readonly object _initLock = new();

    internal static void EnsureInitialized()
    {
        if (_initialized)
        {
            return;
        }

        lock (_initLock)
        {
            if (_initialized)
            {
                return;
            }

            var apiPtr = NativeMethods.FoundryLocalGetApi(NativeMethods.ApiVersion);
            if (apiPtr == IntPtr.Zero)
            {
                throw new InvalidOperationException(
                    $"FoundryLocalGetApi returned null for version {NativeMethods.ApiVersion}.");
            }

            Root = Marshal.PtrToStructure<FlApi>(apiPtr);

            var p = Root.GetItemApi();
            if (p != IntPtr.Zero)
            {
                Item = Marshal.PtrToStructure<FlItemApi>(p);
            }

            p = Root.GetInferenceApi();
            if (p != IntPtr.Zero)
            {
                Inference = Marshal.PtrToStructure<FlInferenceApi>(p);
            }

            p = Root.GetConfigurationApi();
            if (p != IntPtr.Zero)
            {
                Config = Marshal.PtrToStructure<FlConfigurationApi>(p);
            }

            p = Root.GetCatalogApi();
            if (p != IntPtr.Zero)
            {
                Catalog = Marshal.PtrToStructure<FlCatalogApi>(p);
            }

            p = Root.GetModelApi();
            if (p != IntPtr.Zero)
            {
                Model = Marshal.PtrToStructure<FlModelApi>(p);
            }

            _initialized = true;
        }
    }

    internal static void CheckStatus(IntPtr status)
    {
        if (status == IntPtr.Zero)
        {
            return;
        }

        var code = Root.StatusGetErrorCode(status);
        var msgPtr = Root.StatusGetErrorMessage(status);
        var msg = msgPtr == IntPtr.Zero ? "Unknown error" : Detail.Utf8.PtrToString(msgPtr) ?? "Unknown error";
        Root.StatusRelease(status);

        if (code == FlErrorCode.OperationCancelled)
        {
            throw new OperationCanceledException(msg);
        }

        throw new Microsoft.AI.Foundry.Local.FoundryLocalException(msg);
    }

    /// <summary>
    /// Finalizer-safe native handle release. Skips when the runtime is shutting down
    /// (the native DLL may already be unloaded) and swallows any exception (finalizers
    /// must not throw). Intended only for use from <c>~Type()</c>; <c>Dispose()</c>
    /// paths should call the release delegate directly.
    /// </summary>
    internal static void FinalizeRelease(IntPtr ptr, Action<IntPtr> release)
    {
        if (ptr == IntPtr.Zero || Environment.HasShutdownStarted)
        {
            return;
        }

        try { release(ptr); } catch { }
    }
}

// ===================================================================
// Static entry point
// ===================================================================

public static class FoundryLocal
{
    public static string GetVersionString()
    {
        var ptr = NativeMethods.FoundryLocalGetVersionString();
        return ptr == IntPtr.Zero ? string.Empty : Utf8.PtrToString(ptr) ?? string.Empty;
    }
}

// ===================================================================
// Configuration — fluent builder, owns flConfiguration*
// ===================================================================

public sealed class Configuration : IDisposable
{
    internal IntPtr Ptr { get; private set; }
    private bool _disposed;

    public Configuration(string appName)
    {
        Api.EnsureInitialized();
        var status = Api.Config.Create(appName, out var ptr);
        Api.CheckStatus(status);
        Ptr = ptr;
    }

    public Configuration SetAppDataDir(string dir)
    {
        Api.CheckStatus(Api.Config.SetAppDataDir(Ptr, dir));
        return this;
    }

    public Configuration SetLogsDir(string dir)
    {
        Api.CheckStatus(Api.Config.SetLogsDir(Ptr, dir));
        return this;
    }

    public Configuration SetModelCacheDir(string dir)
    {
        Api.CheckStatus(Api.Config.SetModelCacheDir(Ptr, dir));
        return this;
    }

    public Configuration SetDefaultLogLevel(FlLogLevel level)
    {
        Api.CheckStatus(Api.Config.SetDefaultLogLevel(Ptr, level));
        return this;
    }

    public Configuration AddCatalogUrl(string url, string? filterOverride = null)
    {
        Api.CheckStatus(Api.Config.AddCatalogUrl(Ptr, url, filterOverride));
        return this;
    }

    public Configuration AddWebServiceEndpoint(string url)
    {
        Api.CheckStatus(Api.Config.AddWebServiceEndpoint(Ptr, url));
        return this;
    }

    public Configuration SetAdditionalOptions(IntPtr options)
    {
        Api.CheckStatus(Api.Config.SetAdditionalOptions(Ptr, options));
        return this;
    }

    public Configuration SetExternalServiceUrl(string url)
    {
        Api.CheckStatus(Api.Config.SetExternalServiceUrl(Ptr, url));
        return this;
    }

    public Configuration SetCatalogRegion(string region)
    {
        Api.CheckStatus(Api.Config.SetCatalogRegion(Ptr, region));
        return this;
    }

    public void Dispose()
    {
        if (!_disposed && Ptr != IntPtr.Zero)
        {
            Api.Config.Release(Ptr);
            Ptr = IntPtr.Zero;
            _disposed = true;
        }

        GC.SuppressFinalize(this);
    }

    ~Configuration()
    {
        if (!_disposed)
        {
            Api.FinalizeRelease(Ptr, Api.Config.Release.Invoke);
        }
    }
}

// ===================================================================
// Manager — owns flManager*, created from Configuration
// ===================================================================

public sealed class Manager : IDisposable
{
    internal IntPtr Ptr { get; private set; }
    private bool _disposed;

    public Manager(Configuration config)
    {
        Api.EnsureInitialized();
        var status = Api.Root.ManagerCreate(config.Ptr, out var ptr);
        Api.CheckStatus(status);
        Ptr = ptr;
    }

    public Catalog GetCatalog()
    {
        var status = Api.Root.ManagerGetCatalog(Ptr, out var catalogPtr);
        Api.CheckStatus(status);
        return new Catalog(catalogPtr);
    }

    public void StartService()
    {
        Api.CheckStatus(Api.Root.ManagerWebServiceStart(Ptr));
    }

    public void StopService()
    {
        Api.CheckStatus(Api.Root.ManagerWebServiceStop(Ptr));
    }

    public string[] GetServiceUrls()
    {
        var status = Api.Root.ManagerWebServiceUrls(Ptr, out var urlsPtr, out var count);
        Api.CheckStatus(status);

        var numUrls = (int)(ulong)count;
        var urls = new string[numUrls];
        for (int i = 0; i < numUrls; i++)
        {
            var strPtr = Marshal.ReadIntPtr(urlsPtr, i * IntPtr.Size);
            urls[i] = Utf8.PtrToString(strPtr) ?? string.Empty;
        }

        return urls;
    }

    public EpInfo[] GetDiscoverableEps()
    {
        var status = Api.Root.ManagerGetDiscoverableEps(Ptr, out var epsPtr, out var count);
        Api.CheckStatus(status);

        var numEps = (int)(ulong)count;
        var result = new EpInfo[numEps];

        if (numEps > 0)
        {
            var structSize = Marshal.SizeOf<FlEpInfo>();
            for (int i = 0; i < numEps; i++)
            {
                var entryPtr = IntPtr.Add(epsPtr, i * structSize);
                var entry = Marshal.PtrToStructure<FlEpInfo>(entryPtr);
                result[i] = new EpInfo
                {
                    Name = Utf8.PtrToString(entry.Name) ?? string.Empty,
                    IsRegistered = entry.IsRegistered,
                };
            }
        }

        return result;
    }

    public void DownloadAndRegisterEps(string[]? epNames, FlEpProgressCallback? callback)
    {
        IntPtr namesArray = IntPtr.Zero;
        UIntPtr nameCount = UIntPtr.Zero;
        GCHandle[]? handles = null;
        GCHandle ptrHandle = default;

        try
        {
            if (epNames != null && epNames.Length > 0)
            {
                handles = new GCHandle[epNames.Length];
                var ptrs = new IntPtr[epNames.Length];

                for (int i = 0; i < epNames.Length; i++)
                {
                    var bytes = System.Text.Encoding.UTF8.GetBytes(epNames[i] + '\0');
                    handles[i] = GCHandle.Alloc(bytes, GCHandleType.Pinned);
                    ptrs[i] = handles[i].AddrOfPinnedObject();
                }

                ptrHandle = GCHandle.Alloc(ptrs, GCHandleType.Pinned);
                namesArray = ptrHandle.AddrOfPinnedObject();
                nameCount = (UIntPtr)epNames.Length;

                var status = Api.Root.ManagerDownloadAndRegisterEps(Ptr, namesArray, nameCount, callback, IntPtr.Zero);
                Api.CheckStatus(status);
            }
            else
            {
                var status = Api.Root.ManagerDownloadAndRegisterEps(Ptr, IntPtr.Zero, UIntPtr.Zero, callback, IntPtr.Zero);
                Api.CheckStatus(status);
            }
        }
        finally
        {
            if (ptrHandle.IsAllocated)
            {
                ptrHandle.Free();
            }

            if (handles != null)
            {
                foreach (var h in handles)
                {
                    if (h.IsAllocated)
                    {
                        h.Free();
                    }
                }
            }
        }
    }

    public bool IsEpDownloadInProgress()
    {
        return Api.Root.ManagerIsEpDownloadInProgress(Ptr);
    }

    public void Shutdown()
    {
        Api.CheckStatus(Api.Root.ManagerShutdown(Ptr));
    }

    public bool IsShutdownRequested()
    {
        return Api.Root.ManagerIsShutdownRequested(Ptr);
    }

    public void Dispose()
    {
        if (!_disposed && Ptr != IntPtr.Zero)
        {
            Api.Root.ManagerRelease(Ptr);
            Ptr = IntPtr.Zero;
            _disposed = true;
        }

        GC.SuppressFinalize(this);
    }

    ~Manager()
    {
        if (!_disposed)
        {
            Api.FinalizeRelease(Ptr, Api.Root.ManagerRelease.Invoke);
        }
    }
}

// ===================================================================
// Catalog — non-owning (lifetime tied to Manager)
// ===================================================================

public sealed class Catalog
{
    internal IntPtr Ptr { get; }

    internal Catalog(IntPtr ptr) => Ptr = ptr;

    public string GetName()
    {
        var status = Api.Catalog.GetName(Ptr, out var namePtr);
        Api.CheckStatus(status);
        return Utf8.PtrToString(namePtr) ?? string.Empty;
    }

    public ModelList GetModels()
    {
        var status = Api.Catalog.GetModels(Ptr, out var ptr);
        Api.CheckStatus(status);
        return new ModelList(ptr);
    }

    public Model? GetModel(string alias)
    {
        var status = Api.Catalog.GetModel(Ptr, alias, out var ptr);
        Api.CheckStatus(status);
        return ptr == IntPtr.Zero ? null : new Model(ptr);
    }

    public Model? GetModelVariant(string modelId)
    {
        var status = Api.Catalog.GetModelVariant(Ptr, modelId, out var ptr);
        Api.CheckStatus(status);
        return ptr == IntPtr.Zero ? null : new Model(ptr);
    }

    public Model GetLatestVersion(Model model)
    {
        var status = Api.Catalog.GetLatestVersion(Ptr, model.Ptr, out var ptr);
        Api.CheckStatus(status);

        if (ptr == IntPtr.Zero)
        {
            // Defensive: native should already have returned INVALID_ARGUMENT (which
            // CheckStatus throws above). Reaching here means the input IModel was not
            // produced by this catalog — user error, since IModel instances should
            // only come from Catalog APIs.
            throw new FoundryLocalException(
                "GetLatestVersion returned no model. The IModel argument was not produced by this catalog.");
        }

        return new Model(ptr);
    }

    public ModelList GetCachedModels()
    {
        var status = Api.Catalog.GetCachedModels(Ptr, out var ptr);
        Api.CheckStatus(status);
        return new ModelList(ptr);
    }

    public ModelList GetLoadedModels()
    {
        var status = Api.Catalog.GetLoadedModels(Ptr, out var ptr);
        Api.CheckStatus(status);
        return new ModelList(ptr);
    }
}

// ===================================================================
// ModelInfo — non-owning read-only view (lifetime tied to Model)
// ===================================================================

public sealed class ModelInfo
{
    private readonly IntPtr _ptr;

    internal ModelInfo(IntPtr ptr) => _ptr = ptr;

    // Core identity (always present — never null)
    public string Id => Utf8.PtrToString(Api.Model.InfoGetId(_ptr))!;
    public string Name => Utf8.PtrToString(Api.Model.InfoGetName(_ptr))!;
    public int Version => Api.Model.InfoGetVersion(_ptr);
    public string Alias => Utf8.PtrToString(Api.Model.InfoGetAlias(_ptr))!;
    public string? Uri => Utf8.PtrToString(Api.Model.InfoGetUri(_ptr));
    public FlDeviceType DeviceType => Api.Model.InfoGetDeviceType(_ptr);
    public string? ExecutionProvider => Utf8.PtrToString(Api.Model.InfoGetExecutionProvider(_ptr));
    public string? Task => Utf8.PtrToString(Api.Model.InfoGetTask(_ptr));

    // Generic property accessors
    public string? GetStringProperty(string key) =>
        Utf8.PtrToString(Api.Model.InfoGetStringProperty(_ptr, key));

    public long GetIntProperty(string key, long defaultValue = -1) =>
        Api.Model.InfoGetIntProperty(_ptr, key, defaultValue);

    // Typed convenience properties
    public string? DisplayName => GetStringProperty(ModelProperties.DisplayName);
    public string? ModelType => GetStringProperty(ModelProperties.ModelType);
    public string? Publisher => GetStringProperty(ModelProperties.Publisher);
    public string? License => GetStringProperty(ModelProperties.License);
    public string? LicenseDescription => GetStringProperty(ModelProperties.LicenseDescription);
    public string? ModelProvider => GetStringProperty(ModelProperties.ModelProvider);
    public string? MinFlVersion => GetStringProperty(ModelProperties.MinFlVersion);
    public string? ParentUri => GetStringProperty(ModelProperties.ParentUri);
    public string? ToolCallStart => GetStringProperty(ModelProperties.ToolCallStart);
    public string? ToolCallEnd => GetStringProperty(ModelProperties.ToolCallEnd);

    public long SupportsToolCalling => GetIntProperty(ModelProperties.SupportsToolCalling, -1);
    public long FilesizeMb => GetIntProperty(ModelProperties.FileSizeMb, -1);
    public long MaxOutputTokens => GetIntProperty(ModelProperties.MaxOutputTokens, -1);
    public long CreatedAtUnix => GetIntProperty(ModelProperties.CreatedAtUnix, 0);
    public long IsTestModel => GetIntProperty(ModelProperties.IsTestModel, 0);

    /// <summary>
    /// Enumerates the model's default/recommended settings as a key/value dictionary.
    /// Returns null if the model has no settings declared.
    /// </summary>
    public Dictionary<string, string?>? GetModelSettings()
    {
        var kvpsPtr = Api.Model.InfoGetModelSettings(_ptr);
        if (kvpsPtr == IntPtr.Zero)
        {
            return null;
        }

        Api.Root.GetKeyValuePairs(kvpsPtr, out var keysPtr, out var valuesPtr, out var count);
        var numEntries = (int)(ulong)count;
        if (numEntries == 0)
        {
            return new Dictionary<string, string?>();
        }

        var result = new Dictionary<string, string?>(numEntries);
        unsafe
        {
            var keys = (IntPtr*)keysPtr;
            var values = (IntPtr*)valuesPtr;
            for (int i = 0; i < numEntries; i++)
            {
                var key = Utf8.PtrToString(keys[i]);
                if (key == null)
                {
                    continue;
                }

                result[key] = Utf8.PtrToString(values[i]);
            }
        }

        return result;
    }
}

// ===================================================================
// Model — non-owning (lifetime tied to ModelList/Catalog)
// ===================================================================

public sealed class Model
{
    internal IntPtr Ptr { get; }

    internal Model(IntPtr ptr) => Ptr = ptr;

    public ModelInfo GetInfo()
    {
        var status = Api.Model.GetInfo(Ptr, out var infoPtr);
        Api.CheckStatus(status);
        return new ModelInfo(infoPtr);
    }

    public bool IsCached
    {
        get
        {
            var status = Api.Model.IsCached(Ptr, out var cached);
            Api.CheckStatus(status);
            return cached != 0;
        }
    }

    public bool IsLoaded
    {
        get
        {
            var status = Api.Model.IsLoaded(Ptr, out var loaded);
            Api.CheckStatus(status);
            return loaded != 0;
        }
    }

    public string? GetPath()
    {
        var status = Api.Model.GetPath(Ptr, out var pathPtr);
        Api.CheckStatus(status);
        return Utf8.PtrToString(pathPtr);
    }

    public void Download(Func<float, int>? progress = null)
    {
        FlProgressCallback? nativeCallback = null;
        if (progress != null)
        {
            nativeCallback = (value, _) => progress(value);
        }

        var status = Api.Model.Download(Ptr, nativeCallback, IntPtr.Zero);
        Api.CheckStatus(status);
    }

    public void Load()
    {
        Api.CheckStatus(Api.Model.Load(Ptr));
    }

    public void Unload()
    {
        Api.CheckStatus(Api.Model.Unload(Ptr));
    }

    public void RemoveFromCache()
    {
        Api.CheckStatus(Api.Model.RemoveFromCache(Ptr));
    }

    public ModelList GetVariants()
    {
        var status = Api.Model.GetVariants(Ptr, out var ptr);
        Api.CheckStatus(status);
        return new ModelList(ptr);
    }

    public void SelectVariant(Model variant)
    {
        Api.CheckStatus(Api.Model.SelectVariant(Ptr, variant.Ptr));
    }
}

// ===================================================================
// ModelList — owns flModelList*, disposes it
// ===================================================================

public sealed class ModelList : IDisposable
{
    private IntPtr _ptr;
    private bool _disposed;
    private readonly List<Model> _models;

    internal ModelList(IntPtr ptr)
    {
        _ptr = ptr;
        var count = (int)(ulong)Api.Root.ModelListSize(ptr);
        _models = new List<Model>(count);
        for (int i = 0; i < count; i++)
        {
            _models.Add(new Model(Api.Root.ModelListGetAt(ptr, (UIntPtr)i)));
        }
    }

    public IReadOnlyList<Model> Models => _models;

    public void Dispose()
    {
        if (!_disposed && _ptr != IntPtr.Zero)
        {
            Api.Root.ModelListRelease(_ptr);
            _ptr = IntPtr.Zero;
            _disposed = true;
        }

        GC.SuppressFinalize(this);
    }

    ~ModelList()
    {
        if (!_disposed)
        {
            Api.FinalizeRelease(_ptr, Api.Root.ModelListRelease.Invoke);
        }
    }
}

// ===================================================================
// NOTE: The Item / Request / Response / *Content types that previously lived here
// were removed because the public hierarchies in Microsoft.AI.Foundry.Local
// (Items/Item.cs, Request.cs, Response.cs) are the single canonical wrappers used
// by both user code and NativeRequestRunner. Do not reintroduce parallel internal
// copies — extend the public types instead.
// ===================================================================

// ===================================================================
// Session — owns flSession*, created from a loaded Model
// ===================================================================

public sealed class Session : IDisposable
{
    internal IntPtr Ptr { get; private set; }
    private bool _disposed;
    private FlStreamingCallback? _callbackRef;  // prevent GC

    public Session(Model model)
    {
        Api.EnsureInitialized();
        var status = Api.Inference.SessionCreate(model.Ptr, out var ptr);
        Api.CheckStatus(status);
        Ptr = ptr;
    }

    /// <summary>Add a tool definition to the session. The session copies the data.</summary>
    public Session AddToolDefinition(string name, string description, string jsonSchema)
    {
        var nameNative = Utf8.StringToCoTaskMem(name);
        var descNative = Utf8.StringToCoTaskMem(description);
        var schemaNative = Utf8.StringToCoTaskMem(jsonSchema);
        try
        {
            var toolDef = new FlToolDefinition
            {
                Version = NativeMethods.ApiVersion,
                Name = nameNative,
                Description = descNative,
                JsonSchema = schemaNative,
            };
            Api.CheckStatus(Api.Inference.SessionAddToolDefinition(Ptr, ref toolDef));
        }
        finally
        {
            Marshal.FreeCoTaskMem(nameNative);
            Marshal.FreeCoTaskMem(descNative);
            Marshal.FreeCoTaskMem(schemaNative);
        }

        return this;
    }

    /// <summary>
    /// Remove a previously-added tool definition by name.
    /// Returns true if a matching tool was found and removed, false if no tool with that name was registered.
    /// </summary>
    public bool RemoveToolDefinition(string toolName)
    {
        Api.CheckStatus(Api.Inference.SessionRemoveToolDefinition(Ptr, toolName, out var removed));
        return removed;
    }

    /// <summary>Set a streaming callback. Pass null to unset.</summary>
    public Session SetStreamingCallback(FlStreamingCallback? callback)
    {
        _callbackRef = callback;
        Api.CheckStatus(Api.Inference.SessionSetStreamingCallback(Ptr, callback, IntPtr.Zero));
        return this;
    }

    /// <summary>Set session-level inference options. Applies to all subsequent ProcessRequest calls.</summary>
    public Session SetOptions(IntPtr options)
    {
        Api.CheckStatus(Api.Inference.SessionSetOptions(Ptr, options));
        return this;
    }

    /// <summary>
    /// Process a request. Returns a new Response pointer. Caller owns it.
    /// </summary>
    internal IntPtr ProcessRequest(IntPtr requestPtr)
    {
        IntPtr responsePtr = IntPtr.Zero;
        Api.CheckStatus(Api.Inference.SessionProcessRequest(Ptr, requestPtr, ref responsePtr));
        return responsePtr;
    }

    /// <summary>Get the number of completed turns in the session.</summary>
    public ulong TurnCount => (ulong)Api.Inference.SessionGetTurnCount(Ptr);

    /// <summary>
    /// Undo the last <paramref name="count"/> turns: rewinds the generator and removes
    /// the turns' messages from history. If all turns are undone, the cached generator is destroyed.
    /// </summary>
    public void UndoTurns(ulong count)
    {
        Api.CheckStatus(Api.Inference.SessionUndoTurns(Ptr, checked((UIntPtr)count)));
    }

    public void Dispose()
    {
        if (!_disposed && Ptr != IntPtr.Zero)
        {
            _callbackRef = null;
            Api.Inference.SessionRelease(Ptr);
            Ptr = IntPtr.Zero;
            _disposed = true;
        }

        GC.SuppressFinalize(this);
    }

    ~Session()
    {
        if (!_disposed)
        {
            Api.FinalizeRelease(Ptr, Api.Inference.SessionRelease.Invoke);
        }
    }
}
