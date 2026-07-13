// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using Microsoft.Extensions.Logging;

using NativeModel = Microsoft.AI.Foundry.Local.Detail.Native.Model;

public class Model : IModel
{
    private readonly ILogger _logger;
    internal NativeModel NativeModel { get; }

    private ModelInfo? _info;
    private IReadOnlyList<IModel>? _variants;

    public string Id => NativeModel.GetInfo().Id;
    public string Alias => NativeModel.GetInfo().Alias;

    public ModelInfo Info => _info ??= ModelInfo.FromNative(NativeModel);

    public IReadOnlyList<IModel> Variants
    {
        get
        {
            if (_variants == null)
            {
                using var list = NativeModel.GetVariants();
                _variants = list.Models.Select(m => (IModel)new Model(m, _logger)).ToList();
            }

            return _variants;
        }
    }

    internal Model(NativeModel nativeModel, ILogger logger)
    {
        NativeModel = nativeModel;
        _logger = logger;
    }

    public void SelectVariant(IModel variant)
    {
        var model = (Model)variant;
        NativeModel.SelectVariant(model.NativeModel);
    }

    public async Task<bool> IsCachedAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () => NativeModel.IsCached,
            "Error checking if model is cached", _logger, ct).ConfigureAwait(false);
    }

    public async Task<bool> IsLoadedAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () => NativeModel.IsLoaded,
            "Error checking if model is loaded", _logger, ct).ConfigureAwait(false);
    }

    public async Task<string> GetPathAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () =>
            {
                var path = NativeModel.GetPath();
                return path ?? throw new FoundryLocalException(
                    $"Error getting path for model {Id}. Has it been downloaded?");
            },
            "Error getting path for model", _logger, ct).ConfigureAwait(false);
    }

    public async Task DownloadAsync(Action<float>? downloadProgress = null,
                                    CancellationToken? ct = null)
    {
        await Utils.CallWithExceptionHandlingAsync(
            () =>
            {
                Func<float, int>? progressFunc = (value) =>
                {
                    downloadProgress?.Invoke(value);
                    return (ct?.IsCancellationRequested ?? false) ? 1 : 0; // 0 = continue, 1 = cancel
                };

                NativeModel.Download(progressFunc);

                ct?.ThrowIfCancellationRequested();
            },
            $"Error downloading model {Id}", _logger, ct).ConfigureAwait(false);
    }

    public async Task LoadAsync(CancellationToken? ct = null)
    {
        await Utils.CallWithExceptionHandlingAsync(
            () => NativeModel.Load(),
            "Error loading model", _logger, ct).ConfigureAwait(false);
    }

    public async Task UnloadAsync(CancellationToken? ct = null)
    {
        await Utils.CallWithExceptionHandlingAsync(
            () => NativeModel.Unload(),
            "Error unloading model", _logger, ct).ConfigureAwait(false);
    }

    public async Task RemoveFromCacheAsync(CancellationToken? ct = null)
    {
        await Utils.CallWithExceptionHandlingAsync(
            () => NativeModel.RemoveFromCache(),
            $"Error removing model {Id} from cache", _logger, ct).ConfigureAwait(false);
    }

    [System.Obsolete("Use new ChatSession(model) instead.", error: false)]
    public async Task<OpenAIChatClient> GetChatClientAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            async () =>
            {
                if (!await IsLoadedAsync(ct).ConfigureAwait(false))
                {
                    throw new FoundryLocalException($"Model {Id} is not loaded. Call LoadAsync first.");
                }

#pragma warning disable CS0618 // OpenAIChatClient is obsolete
                return new OpenAIChatClient(Id, NativeModel);
#pragma warning restore CS0618
            },
            "Error getting chat client for model", _logger).ConfigureAwait(false);
    }

    [System.Obsolete("Use new AudioSession(model) instead.", error: false)]
    public async Task<OpenAIAudioClient> GetAudioClientAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            async () =>
            {
                if (!await IsLoadedAsync(ct).ConfigureAwait(false))
                {
                    throw new FoundryLocalException($"Model {Id} is not loaded. Call LoadAsync first.");
                }

#pragma warning disable CS0618 // OpenAIAudioClient is obsolete
                return new OpenAIAudioClient(Id, NativeModel);
#pragma warning restore CS0618
            },
            "Error getting audio client for model", _logger).ConfigureAwait(false);
    }

    [System.Obsolete("Use new EmbeddingsSession(model) instead.", error: false)]
    public async Task<OpenAIEmbeddingClient> GetEmbeddingClientAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            async () =>
            {
                if (!await IsLoadedAsync(ct).ConfigureAwait(false))
                {
                    throw new FoundryLocalException($"Model {Id} is not loaded. Call LoadAsync first.");
                }

#pragma warning disable CS0618 // OpenAIEmbeddingClient is obsolete
                return new OpenAIEmbeddingClient(Id, NativeModel);
#pragma warning restore CS0618
            },
            "Error getting embedding client for model", _logger).ConfigureAwait(false);
    }
}
