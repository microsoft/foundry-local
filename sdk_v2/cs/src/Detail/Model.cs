// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Runtime.ExceptionServices;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.Extensions.Logging;

using NativeModel = Microsoft.AI.Foundry.Local.Detail.Native.Model;

public class Model : IModel
{
    private readonly ILogger _logger;
    private readonly ManagerLifetime _nativeLifetime;
    internal NativeModel NativeModel { get; }
    internal ManagerLifetime NativeLifetime => _nativeLifetime;
    internal Action? BeforeNativeCallForTest { get; set; }

    private IReadOnlyList<IModel>? _variants;

    public string Id => WithManagerLock(() => NativeModel.GetInfo().Id);
    public string Alias => WithManagerLock(() => NativeModel.GetInfo().Alias);

    // The native model is the source of truth. Reading fresh every time keeps metadata correct after
    // SelectVariant / download / cache changes. Each read returns a point-in-time ModelInfo snapshot.
    public ModelInfo Info => WithManagerLock(() => ModelInfo.FromNative(NativeModel));

    public string? GetStringProperty(string key)
    {
        Detail.Throw.IfContainsEmbeddedNul(key);
        return WithManagerLock(() => NativeModel.GetInfo().GetStringProperty(key));
    }

    public long GetIntProperty(string key, long defaultValue = 0)
    {
        Detail.Throw.IfContainsEmbeddedNul(key);
        return WithManagerLock(() => NativeModel.GetInfo().GetIntProperty(key, defaultValue));
    }

    public IReadOnlyList<IModel> Variants
    {
        get
        {
            using var lease = AcquireManagerLease(trackReentrancy: true);
            BeforeNativeCallForTest?.Invoke();
            if (_variants == null)
            {
                using var list = NativeModel.GetVariants();
                _variants = list.Models.Select(CreateModel).ToList();
            }

            return _variants;
        }
    }

    internal Model(NativeModel nativeModel, ILogger logger, ManagerLifetime nativeLifetime)
    {
        NativeModel = nativeModel;
        _logger = logger;
        _nativeLifetime = nativeLifetime;
    }

    internal T WithNativeModel<T>(Func<NativeModel, T> operation) =>
        WithManagerLock(() => operation(NativeModel));

    internal ManagerLifetime.Lease AcquireManagerLease(bool trackReentrancy = false) =>
        _nativeLifetime.Acquire(this, trackReentrancy);

    internal bool HasSameManager(Model other) => ReferenceEquals(_nativeLifetime, other._nativeLifetime);

    public void SelectVariant(IModel variant)
    {
        var model = (Model)variant;
        if (!HasSameManager(model))
        {
            throw new ArgumentException("Variant must belong to the same manager.", nameof(variant));
        }
        WithManagerLock(() => NativeModel.SelectVariant(model.NativeModel));
    }

    public async Task<bool> IsCachedAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () => WithManagerLock(() => NativeModel.IsCached),
            "Error checking if model is cached", _logger, ct).ConfigureAwait(false);
    }

    public async Task<bool> IsLoadedAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () => WithManagerLock(() => NativeModel.IsLoaded),
            "Error checking if model is loaded", _logger, ct).ConfigureAwait(false);
    }

    public async Task<string> GetPathAsync(CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () =>
            {
                var path = WithManagerLock(() => NativeModel.GetPath());
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
                Exception? callbackException = null;
                Func<float, int>? progressFunc = (value) =>
                {
                    try
                    {
                        downloadProgress?.Invoke(value);
                        return (ct?.IsCancellationRequested ?? false) ? 1 : 0; // 0 = continue, 1 = cancel
                    }
                    catch (Exception ex)
                    {
                        callbackException = ex;
                        return 1;
                    }
                };

                try
                {
                    WithManagerLock(() => NativeModel.Download(progressFunc));
                }
                catch when (callbackException != null)
                {
                    ExceptionDispatchInfo.Capture(callbackException).Throw();
                }
                if (callbackException != null)
                {
                    ExceptionDispatchInfo.Capture(callbackException).Throw();
                }

                ct?.ThrowIfCancellationRequested();
            },
            $"Error downloading model {Id}", _logger, ct).ConfigureAwait(false);
    }

    public async Task LoadAsync(CancellationToken? ct = null)
    {
        await Utils.CallWithExceptionHandlingAsync(
            () => WithManagerLock(() => NativeModel.Load()),
            "Error loading model", _logger, ct).ConfigureAwait(false);
    }

    public async Task UnloadAsync(CancellationToken? ct = null)
    {
        await Utils.CallWithExceptionHandlingAsync(
            () => WithManagerLock(() => NativeModel.Unload()),
            "Error unloading model", _logger, ct).ConfigureAwait(false);
    }

    public async Task RemoveFromCacheAsync(CancellationToken? ct = null)
    {
        await Utils.CallWithExceptionHandlingAsync(
            () => WithManagerLock(() => NativeModel.RemoveFromCache()),
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
                return new OpenAIChatClient(Id, this);
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
                return new OpenAIAudioClient(Id, this);
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
                return new OpenAIEmbeddingClient(Id, this);
#pragma warning restore CS0618
            },
            "Error getting embedding client for model", _logger).ConfigureAwait(false);
    }

    private IModel CreateModel(NativeModel model) =>
        new Model(model, _logger, _nativeLifetime);

    private T WithManagerLock<T>(Func<T> operation)
    {
        using var lease = AcquireManagerLease(trackReentrancy: true);
        BeforeNativeCallForTest?.Invoke();
        return operation();
    }

    private void WithManagerLock(Action operation)
    {
        using var lease = AcquireManagerLease(trackReentrancy: true);
        BeforeNativeCallForTest?.Invoke();
        operation();
    }
}
