// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------
namespace Microsoft.AI.Foundry.Local;

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.Extensions.Logging;

using NativeConfig = Microsoft.AI.Foundry.Local.Detail.Native.Configuration;
using NativeManager = Microsoft.AI.Foundry.Local.Detail.Native.Manager;

public class FoundryLocalManager : IDisposable
{
    private static volatile FoundryLocalManager? instance;
    private static readonly AsyncLock asyncLock = new();

    internal static readonly string AssemblyVersion =
        typeof(FoundryLocalManager).Assembly.GetName().Version?.ToString() ?? "unknown";

    private readonly Configuration _config;
    private NativeConfig _nativeConfig = default!;
    private NativeManager _nativeManager = default!;
    private Catalog? _catalog;
    private readonly AsyncLock _lock = new();
    private int _disposed;
    private readonly ILogger _logger;

    private static readonly char[] s_urlSeparator = { ';' };

    internal Configuration Configuration => _config;
    internal ILogger Logger => _logger;
    internal NativeManager NativeManager => _nativeManager;

    public static bool IsInitialized => instance != null;
    public static FoundryLocalManager Instance => instance ??
        throw new FoundryLocalException("FoundryLocalManager has not been created. Call CreateAsync first.");

    /// <summary>
    /// Bound Urls if the web service has been started. Null otherwise.
    /// See <see cref="StartWebServiceAsync"/>.
    /// </summary>
    public string[]? Urls { get; private set; }

    /// <summary>
    /// Create the <see cref="FoundryLocalManager"/> singleton instance.
    /// </summary>
    /// <param name="configuration">Configuration to use.</param>
    /// <param name="logger">Application logger to use.
    /// Use Microsoft.Extensions.Logging.NullLogger.Instance if you wish to ignore log output from the SDK.
    /// </param>
    /// <param name="ct">Optional cancellation token for the initialization.</param>
    /// <returns>Task creating the instance.</returns>
    /// <exception cref="FoundryLocalException"></exception>
    public static async Task CreateAsync(Configuration configuration, ILogger logger,
                                         CancellationToken? ct = null)
    {
        using var disposable = await asyncLock.LockAsync().ConfigureAwait(false);

        if (instance != null)
        {
            throw new FoundryLocalException("FoundryLocalManager has already been created.", logger);
        }

        FoundryLocalManager? manager = null;
        try
        {
            manager = new FoundryLocalManager(configuration, logger);
            await manager.InitializeAsync(ct).ConfigureAwait(false);

#pragma warning disable IDISP003 // Dispose previous before re-assigning
            instance = manager;
            manager = null;
#pragma warning restore IDISP003
        }
        catch (Exception ex)
        {
            manager?.Dispose();

            if (ex is FoundryLocalException or OperationCanceledException)
            {
                throw;
            }

            throw new FoundryLocalException("Error during initialization.", ex, logger);
        }
    }

    /// <summary>
    /// Get the model catalog instance.
    /// </summary>
    /// <param name="ct">Optional cancellation token.</param>
    /// <returns>The model catalog.</returns>
    public async Task<ICatalog> GetCatalogAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(() => GetCatalogImplAsync(ct),
                                                          "Error getting Catalog.", _logger).ConfigureAwait(false);
    }

    /// <summary>
    /// Start the optional web service.
    /// <see cref="Urls"/> is populated with the actual bound Urls after startup.
    /// </summary>
    /// <param name="ct">Optional cancellation token.</param>
    /// <returns>Task starting the web service.</returns>
    public async Task StartWebServiceAsync(CancellationToken? ct = null)
    {
        await Utils.CallWithExceptionHandlingAsync(() => StartWebServiceImplAsync(ct),
                                                   "Error starting web service.", _logger).ConfigureAwait(false);
    }

    /// <summary>
    /// Stops the web service if started.
    /// </summary>
    /// <param name="ct">Optional cancellation token.</param>
    /// <returns>Task stopping the web service.</returns>
    public async Task StopWebServiceAsync(CancellationToken? ct = null)
    {
        await Utils.CallWithExceptionHandlingAsync(() => StopWebServiceImplAsync(ct),
                                                   "Error stopping web service.", _logger).ConfigureAwait(false);
    }

    /// <summary>
    /// Discovers all available execution provider bootstrappers.
    /// Returns metadata about each EP including whether it is already registered.
    /// </summary>
    /// <returns>Array of EP bootstrapper info describing available EPs.</returns>
    public EpInfo[] DiscoverEps()
    {
        return _nativeManager.GetDiscoverableEps();
    }

    /// <summary>
    /// Initiate graceful shutdown of the native manager. Safe to call from any thread. Idempotent.
    /// Sessions in flight will be allowed to complete; new requests will be rejected.
    /// </summary>
    public void Shutdown()
    {
        _nativeManager.Shutdown();
    }

    /// <summary>
    /// Whether <see cref="Shutdown"/> has been called on the native manager.
    /// </summary>
    public bool IsShutdownRequested => _nativeManager.IsShutdownRequested();

    /// <summary>
    /// Downloads and registers all available execution providers.
    /// </summary>
    public async Task<EpDownloadResult> DownloadAndRegisterEpsAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () => DownloadAndRegisterEpsAsyncImpl(names: null, progressCallback: null, ct: ct),
            "Error downloading and registering execution providers.", _logger).ConfigureAwait(false);
    }

    /// <summary>
    /// Downloads and registers the specified execution providers.
    /// </summary>
    public Task<EpDownloadResult> DownloadAndRegisterEpsAsync(IEnumerable<string> names,
                                                              CancellationToken? ct = null)
    {
        return Utils.CallWithExceptionHandlingAsync(
            () => DownloadAndRegisterEpsAsyncImpl(names, progressCallback: null, ct: ct),
            "Error downloading and registering execution providers.", _logger);
    }

    /// <summary>
    /// Downloads and registers all available execution providers, reporting progress.
    /// </summary>
    public async Task<EpDownloadResult> DownloadAndRegisterEpsAsync(Action<string, double> progressCallback,
                                                              CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () => DownloadAndRegisterEpsAsyncImpl(names: null, progressCallback: progressCallback, ct: ct),
            "Error downloading and registering execution providers.", _logger).ConfigureAwait(false);
    }

    /// <summary>
    /// Downloads and registers the specified execution providers, reporting progress.
    /// </summary>
    public async Task<EpDownloadResult> DownloadAndRegisterEpsAsync(IEnumerable<string> names,
                                                                    Action<string, double> progressCallback,
                                                                    CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () => DownloadAndRegisterEpsAsyncImpl(names, progressCallback, ct),
            "Error downloading and registering execution providers.", _logger).ConfigureAwait(false);
    }

    private FoundryLocalManager(Configuration configuration, ILogger logger)
    {
        _config = configuration ?? throw new ArgumentNullException(nameof(configuration));
        _logger = logger;
    }

    private async Task InitializeAsync(CancellationToken? ct = null)
    {
        _config.Validate();

        // Ensure the native DLL resolver is registered before any P/Invoke calls.
        DllLoader.Initialize();

        await Task.Run(() =>
        {
            // Build native configuration
#pragma warning disable IDISP003 // Dispose previous before re-assigning — always null on first call
            _nativeConfig = new NativeConfig(_config.AppName);

            if (!string.IsNullOrEmpty(_config.AppDataDir))
            {
                _nativeConfig.SetAppDataDir(_config.AppDataDir!);
            }

            if (!string.IsNullOrEmpty(_config.ModelCacheDir))
            {
                _nativeConfig.SetModelCacheDir(_config.ModelCacheDir!);
            }

            if (!string.IsNullOrEmpty(_config.LogsDir))
            {
                _nativeConfig.SetLogsDir(_config.LogsDir!);
            }

            _nativeConfig.SetDefaultLogLevel(MapLogLevel(_config.LogLevel));

            if (_config.Web?.Urls != null)
            {
                // Web.Urls can be semicolon-separated
                foreach (var url in _config.Web.Urls.Split(s_urlSeparator, StringSplitOptions.RemoveEmptyEntries))
                {
                    _nativeConfig.AddWebServiceEndpoint(url.Trim());
                }
            }

            // Merge AdditionalSettings with user-supplied entries.
            // Done as a local dict so we don't mutate the user-supplied AdditionalSettings.
            var additionalSettings = new Dictionary<string, string>(StringComparer.Ordinal);
            if (_config.AdditionalSettings != null)
            {
                foreach (var kvp in _config.AdditionalSettings)
                {
                    if (string.IsNullOrEmpty(kvp.Key))
                    {
                        continue;
                    }
                    additionalSettings[kvp.Key] = kvp.Value ?? string.Empty;
                }
            }

            if (additionalSettings.Count > 0)
            {
                // Create a KeyValuePairs from additional settings
                Detail.Native.Api.Root.CreateKeyValuePairs(out var kvpPtr);
                foreach (var kvp in additionalSettings)
                {
                    Detail.Native.Api.Root.AddKeyValuePair(kvpPtr, kvp.Key, kvp.Value);
                }

                _nativeConfig.SetAdditionalOptions(kvpPtr);
                Detail.Native.Api.Root.KeyValuePairsRelease(kvpPtr);
            }

            if (_config.Web?.ExternalUrl != null)
            {
                _nativeConfig.SetExternalServiceUrl(_config.Web.ExternalUrl.ToString());
            }

            if (!string.IsNullOrEmpty(_config.CatalogRegion))
            {
                _nativeConfig.SetCatalogRegion(_config.CatalogRegion!);
            }

            if (_config.CatalogUrls != null)
            {
                foreach (var (url, filter) in _config.CatalogUrls)
                {
                    _nativeConfig.AddCatalogUrl(url, filter);
                }
            }

            // Create the native manager
            _nativeManager = new NativeManager(_nativeConfig);
#pragma warning restore IDISP003
        }, ct ?? CancellationToken.None).ConfigureAwait(false);

        _logger.LogInformation("FoundryLocalManager initialized successfully.");
    }

    private static FlLogLevel MapLogLevel(LogLevel level) => level switch
    {
        LogLevel.Verbose => FlLogLevel.Verbose,
        LogLevel.Debug => FlLogLevel.Debug,
        LogLevel.Information => FlLogLevel.Info,
        LogLevel.Warning => FlLogLevel.Warning,
        LogLevel.Error => FlLogLevel.Error,
        LogLevel.Fatal => FlLogLevel.Fatal,
        _ => FlLogLevel.Warning,
    };

    private async Task<ICatalog> GetCatalogImplAsync(CancellationToken? ct = null)
    {
        if (_catalog == null)
        {
            using var disposable = await _lock.LockAsync().ConfigureAwait(false);

            if (_catalog == null)
            {
                _catalog = await Task.Run(() =>
                {
                    var nativeCatalog = _nativeManager.GetCatalog();
                    return new Catalog(nativeCatalog, _logger);
                }, ct ?? CancellationToken.None).ConfigureAwait(false);
            }
        }

        return _catalog;
    }

    private async Task<EpDownloadResult> DownloadAndRegisterEpsAsyncImpl(
        IEnumerable<string>? names = null,
        Action<string, double>? progressCallback = null,
        CancellationToken? ct = null)
    {
        var beforeEps = DiscoverEps();

        FlEpProgressCallback? nativeCallback = null;
        if (progressCallback != null)
        {
            nativeCallback = (epName, value, _) =>
            {
                if (ct?.IsCancellationRequested ?? false)
                {
                    return 1;
                }

                progressCallback(epName, value);
                return 0;
            };
        }

        var nameArray = names?.ToArray();

        await Task.Run(() =>
        {
            _nativeManager.DownloadAndRegisterEps(nameArray, nativeCallback);
        }, ct ?? CancellationToken.None).ConfigureAwait(false);

        var afterEps = DiscoverEps();

        var registered = afterEps.Where(e => e.IsRegistered && !beforeEps.Any(b => b.Name == e.Name && b.IsRegistered))
                                  .Select(e => e.Name).ToArray();
        var failed = afterEps.Where(e => !e.IsRegistered && (nameArray == null || nameArray.Contains(e.Name)))
                              .Select(e => e.Name)
                              .Except(beforeEps.Where(b => !b.IsRegistered).Select(b => b.Name))
                              .ToArray();

        return new EpDownloadResult
        {
            Success = failed.Length == 0,
            Status = failed.Length == 0 ? "All EPs registered" : "Some EPs failed",
            RegisteredEps = registered,
            FailedEps = failed,
        };
    }

    private async Task StartWebServiceImplAsync(CancellationToken? ct = null)
    {
        using var disposable = await asyncLock.LockAsync().ConfigureAwait(false);

        await Task.Run(() => _nativeManager.StartService(), ct ?? CancellationToken.None).ConfigureAwait(false);

        Urls = _nativeManager.GetServiceUrls();
    }

    private async Task StopWebServiceImplAsync(CancellationToken? ct = null)
    {
        if (Urls == null)
        {
            throw new FoundryLocalException("Web service is not running.", _logger);
        }

        using var disposable = await asyncLock.LockAsync().ConfigureAwait(false);

        await Task.Run(() => _nativeManager.StopService(), ct ?? CancellationToken.None).ConfigureAwait(false);

        Urls = null;
    }

    protected virtual void Dispose(bool disposing)
    {
        // this is possibly overly cautious, but we free native handles here so want to make sure we get it right
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
        {
            return;
        }

        if (disposing)
        {
            if (Urls != null)
            {
                try
                {
                    // Run on a thread-pool thread so that synchronously waiting on the asyncLock
                    // cannot deadlock with an awaited continuation captured on the caller's context.
                    Task.Run(() => StopWebServiceImplAsync()).GetAwaiter().GetResult();
                }
                catch (Exception ex)
                {
                    _logger.LogWarning(ex, "Error stopping web service during Dispose.");
                }
            }

            if (_nativeManager != null)
            {
                try
                {
                    _nativeManager.Shutdown();
                }
                catch (Exception ex)
                {
                    _logger.LogWarning(ex, "Error initiating native manager shutdown during Dispose.");
                }
            }

            _nativeManager?.Dispose();
            _nativeConfig?.Dispose();
            _lock.Dispose();

            // Allow CreateAsync to construct a fresh instance after dispose. The native singleton
            // (Manager::Shutdown + Manager::Instance) already supports re-creation; the C# static
            // field was the only thing keeping the SDK permanently dead after Dispose.
            if (ReferenceEquals(instance, this))
            {
#pragma warning disable IDISP003 // Dispose previous before re-assigning — `this` was just disposed above
                instance = null;
#pragma warning restore IDISP003
            }
        }
    }

    public void Dispose()
    {
        Dispose(disposing: true);
        GC.SuppressFinalize(this);
    }
}
