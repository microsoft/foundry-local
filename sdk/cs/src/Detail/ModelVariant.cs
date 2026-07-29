// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Globalization;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.Extensions.Logging;

internal class ModelVariant : IModel
{
    private readonly IModelLoadManager _modelLoadManager;
    private readonly ICoreInterop _coreInterop;
    private readonly ILogger _logger;

    public ModelInfo Info { get; private set; } // expose the full info record

    // expose a few common properties directly
    public string Id => Info.Id;
    public string Alias => Info.Alias;
    public int Version { get; private set; }  // parsed from Info.Version if possible, else 0

    public IReadOnlyList<IModel> Variants => [this];

    internal ModelVariant(ModelInfo modelInfo, IModelLoadManager modelLoadManager, ICoreInterop coreInterop,
                          ILogger logger)
    {
        Info = modelInfo;
        Version = modelInfo.Version;

        _modelLoadManager = modelLoadManager;
        _coreInterop = coreInterop;
        _logger = logger;

    }

    /// <summary>
    /// Replace the cached <see cref="ModelInfo"/> snapshot in place.
    /// Called by <see cref="Catalog.UpdateModels"/> during incremental refresh
    /// so wrapper identity is preserved across refreshes while still surfacing
    /// fresh metadata (notably <c>Cached</c>) on held references. <c>Id</c>
    /// and <c>Alias</c> are immutable for a given variant; callers must only
    /// invoke this with a <paramref name="modelInfo"/> whose id matches
    /// <see cref="Id"/>. Reference assignment in CLR is atomic, so concurrent
    /// readers observe either the old or new snapshot, never a torn
    /// intermediate.
    /// </summary>
    internal void RefreshInfo(ModelInfo modelInfo)
    {
        Info = modelInfo;
        Version = modelInfo.Version;
    }

    // simpler and always correct to check if loaded from the model load manager
    // this allows for multiple instances of ModelVariant to exist
    public async Task<bool> IsLoadedAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandling(() => IsLoadedImplAsync(ct),
                                                     "Error checking if model is loaded", _logger)
                                                    .ConfigureAwait(false);
    }

    public async Task<bool> IsCachedAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandling(() => IsCachedImplAsync(ct),
                                                     "Error checking if model is cached", _logger)
                                                    .ConfigureAwait(false);
    }

    public async Task<string> GetPathAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandling(() => GetPathImplAsync(ct),
                                                     "Error getting path for model", _logger)
                                                    .ConfigureAwait(false);
    }

    public async Task DownloadAsync(Action<float>? downloadProgress = null,
                                    CancellationToken? ct = null)
    {
        await Utils.CallWithExceptionHandling(() => DownloadImplAsync(downloadProgress, ct),
                                               $"Error downloading model {Id}", _logger)
                                              .ConfigureAwait(false);
    }

    public async Task LoadAsync(CancellationToken? ct = null)
    {
        await Utils.CallWithExceptionHandling(() => _modelLoadManager.LoadAsync(Id, ct),
                                              "Error loading model", _logger)
                                             .ConfigureAwait(false);
    }

    public async Task UnloadAsync(CancellationToken? ct = null)
    {
        await Utils.CallWithExceptionHandling(() => _modelLoadManager.UnloadAsync(Id, ct),
                                              "Error unloading model", _logger)
                                             .ConfigureAwait(false);
    }

    public async Task RemoveFromCacheAsync(CancellationToken? ct = null)
    {
        await Utils.CallWithExceptionHandling(() => RemoveFromCacheImplAsync(ct),
                                              $"Error removing model {Id} from cache", _logger)
                                             .ConfigureAwait(false);
    }

    public async Task<OpenAIChatClient> GetChatClientAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandling(() => GetChatClientImplAsync(ct),
                                                     "Error getting chat client for model", _logger)
                                                    .ConfigureAwait(false);
    }

    public async Task<OpenAIAudioClient> GetAudioClientAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandling(() => GetAudioClientImplAsync(ct),
                                                     "Error getting audio client for model", _logger)
                                                    .ConfigureAwait(false);
    }

    public async Task<OpenAIEmbeddingClient> GetEmbeddingClientAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandling(() => GetEmbeddingClientImplAsync(ct),
                                                     "Error getting embedding client for model", _logger)
                                                    .ConfigureAwait(false);
    }

    private async Task<bool> IsLoadedImplAsync(CancellationToken? ct = null)
    {
        var loadedModels = await _modelLoadManager.ListLoadedModelsAsync(ct).ConfigureAwait(false);
        return loadedModels.Contains(Id);
    }

    private async Task<bool> IsCachedImplAsync(CancellationToken? ct = null)
    {
        var cachedModelIds = await Utils.GetCachedModelIdsAsync(_coreInterop, ct).ConfigureAwait(false);
        return cachedModelIds.Contains(Id);
    }

    private async Task<string> GetPathImplAsync(CancellationToken? ct = null)
    {
        var request = new CoreInteropRequest { Params = new Dictionary<string, string> { { "Model", Id } } };
        var result = await _coreInterop.ExecuteCommandAsync("get_model_path", request, ct).ConfigureAwait(false);
        if (result.Error != null)
        {
            throw new FoundryLocalException(
                $"Error getting path for model {Id}: {result.Error}. Has it been downloaded?");
        }

        var path = result.Data!;
        return path;
    }

    private async Task DownloadImplAsync(Action<float>? downloadProgress = null,
                                         CancellationToken? ct = null)
    {
        var request = new CoreInteropRequest
        {
            Params = new() { { "Model", Id } }
        };

        ICoreInterop.Response? response;
        var useCallbackPath = downloadProgress != null || (ct?.CanBeCanceled ?? false);

        if (useCallbackPath)
        {
            var callback = new ICoreInterop.CallbackFn(progressString =>
            {
                if (ct is CancellationToken cancellationToken)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                }

                if (downloadProgress == null)
                {
                    return;
                }

                if (float.TryParse(progressString,
                                   NumberStyles.Float,
                                   CultureInfo.InvariantCulture,
                                   out var progress))
                {
                    downloadProgress(progress);
                }
            });

            response = await _coreInterop.ExecuteCommandWithCallbackAsync("download_model", request,
                                                                          callback, ct).ConfigureAwait(false);
        }
        else
        {
            response = await _coreInterop.ExecuteCommandAsync("download_model", request, ct).ConfigureAwait(false);
        }

        if (response.Error != null)
        {
            throw new FoundryLocalException($"Error downloading model {Id}: {response.Error}");
        }
    }

    private async Task RemoveFromCacheImplAsync(CancellationToken? ct = null)
    {
        var request = new CoreInteropRequest { Params = new Dictionary<string, string> { { "Model", Id } } };

        var result = await _coreInterop.ExecuteCommandAsync("remove_cached_model", request, ct).ConfigureAwait(false);
        if (result.Error != null)
        {
            throw new FoundryLocalException($"Error removing model {Id} from cache: {result.Error}");
        }
    }

    private async Task<OpenAIChatClient> GetChatClientImplAsync(CancellationToken? ct = null)
    {
        if (!await IsLoadedAsync(ct))
        {
            throw new FoundryLocalException($"Model {Id} is not loaded. Call LoadAsync first.");
        }

        return new OpenAIChatClient(Id);
    }

    private async Task<OpenAIAudioClient> GetAudioClientImplAsync(CancellationToken? ct = null)
    {
        if (!await IsLoadedAsync(ct))
        {
            throw new FoundryLocalException($"Model {Id} is not loaded. Call LoadAsync first.");
        }

        return new OpenAIAudioClient(Id);
    }

    private async Task<OpenAIEmbeddingClient> GetEmbeddingClientImplAsync(CancellationToken? ct = null)
    {
        if (!await IsLoadedAsync(ct))
        {
            throw new FoundryLocalException($"Model {Id} is not loaded. Call LoadAsync first.");
        }

        return new OpenAIEmbeddingClient(Id);
    }

    public void SelectVariant(IModel variant)
    {
        throw new FoundryLocalException(
            $"SelectVariant is not supported on a ModelVariant. " +
            $"Call Catalog.GetModelAsync(\"{Alias}\") to get an IModel with all variants available.");
    }
}
