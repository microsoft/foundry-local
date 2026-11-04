// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.OpenAI;

using System.Runtime.CompilerServices;
using System.Threading.Channels;

using Betalgo.Ranul.OpenAI.ObjectModels.RealtimeModels;

using Microsoft.AI.Foundry.Local;
using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.AI.Foundry.Local.Detail.Interop;

using Api = Microsoft.AI.Foundry.Local.Detail.Native.Api;
using NativeSession = Microsoft.AI.Foundry.Local.Detail.Native.Session;

#pragma warning disable IDISP001 // Dispose created — ownership transfers to Request/Queue
#pragma warning disable IDISP003 // Dispose previous before re-assigning — fields assigned once in StartAsync
#pragma warning disable CA2000   // Dispose objects before losing scope — ownership transfers

/// <summary>
/// Session for real-time audio streaming ASR (Automatic Speech Recognition).
/// Push PCM audio chunks via <see cref="AppendAsync"/> and consume transcription results
/// via <see cref="GetStream"/>.
/// </summary>
[System.Obsolete(
    "LiveAudioTranscriptionSession is deprecated and will be removed at the end of 2026. " +
    "Use AudioSession streaming instead. OpenAI types remain supported for the web-server path.",
    error: false)]
public sealed class LiveAudioTranscriptionSession : IAsyncDisposable
{
    private enum SessionState { Created, Started, Stopped, Disposed }

    private readonly string _modelId;
    private readonly Model _model;
    private readonly object _stateSync = new();
#pragma warning disable IDISP002, IDISP006 // Adapter is disposed through manager registration or Cleanup.
    private readonly SessionRegistrationTarget _registrationTarget;
#pragma warning restore IDISP002, IDISP006

    private SessionState _state = SessionState.Created;
    private ItemQueue? _queue;
    private NativeSession? _session;
    private ManagerLifetime.Lease? _managerLease;
    private ManagerLifetime.SessionRegistration? _managerRegistration;
    private Request? _request;
    private Channel<LiveAudioTranscriptionResponse>? _channel;
    private Task? _processingTask;
    private CancellationTokenSource? _stopCts;
    private int _cleanupStarted;

    /// <summary>
    /// Audio format settings for the streaming session.
    /// </summary>
    public record LiveAudioTranscriptionOptions
    {
        public int SampleRate { get; set; } = 16000;
        public int Channels { get; set; } = 1;
        public int BitsPerSample { get; set; } = 16;
        public string? Language { get; set; }
        public int PushQueueCapacity { get; set; } = 100;
    }

    public LiveAudioTranscriptionOptions Settings { get; } = new();

    internal LiveAudioTranscriptionSession(string modelId, Model model)
    {
        _modelId = modelId;
        _model = model;
        _registrationTarget = new SessionRegistrationTarget(this);
    }

    public Task StartAsync(CancellationToken ct = default)
    {
        lock (_stateSync)
        {
            Detail.Throw.IfDisposed(_state == SessionState.Disposed, this);

            if (_state != SessionState.Created)
            {
                throw new FoundryLocalException($"Session can only be started once (was {_state}).");
            }

            return StartCore(ct);
        }
    }

    private Task StartCore(CancellationToken ct)
    {
        var formatDescriptor = AudioItem.CreateFormatDescriptor("pcm", Settings.SampleRate, Settings.Channels);
        var language = Settings.Language;

        try
        {
            _queue = new ItemQueue();
            _channel = Channel.CreateUnbounded<LiveAudioTranscriptionResponse>(
                new UnboundedChannelOptions
                {
                    SingleWriter = true,
                    SingleReader = true,
                    AllowSynchronousContinuations = true
                });

            _managerLease = _model.AcquireManagerLease();
            _session = new NativeSession(_model.NativeModel);
            _managerRegistration = _model.NativeLifetime.RegisterSession(_registrationTarget);

            var channel = _channel;

            _stopCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            var stopToken = _stopCts.Token;

            FlStreamingCallback streamingCallback = (FlStreamingCallbackData data, IntPtr userData) =>
            {
                bool errored = false;

                try
                {
                    if (data.ItemQueue != IntPtr.Zero)
                    {
                        while (Api.Item.QueueTryPop(data.ItemQueue, out var itemPtr))
                        {
                            using var item = Item.FromNative(itemPtr, ownsHandle: true);

                            LiveAudioTranscriptionResponse? response = null;

                            if (item is SpeechSegmentItem segItem && !string.IsNullOrEmpty(segItem.Text))
                            {
                                // Direct streaming path — per-token segments from AudioSession.
                                // Matches legacy SDK semantics which doesn't conform to either
                                // OAI transcription streaming or OAI realtime API types/semantics.
                                // IsFinal here marks the last message in the stream (set by the
                                // final-Response drain below), not per-segment finality, so we
                                // intentionally ignore SpeechSegmentKind.Final on intermediate segments.
                                response = new LiveAudioTranscriptionResponse
                                {
                                    IsFinal = false,
                                    Content =
                                    [
                                        new ContentPart
                                        {
                                            Text = segItem.Text,
                                            Transcript = segItem.Text
                                        }
                                    ]
                                };
                            }

                            if (response != null)
                            {
                                channel.Writer.TryWrite(response);
                            }
                        }
                    }
                }
                catch (Exception ex)
                {
                    errored = true;
                    channel.Writer.TryComplete(
                        new FoundryLocalException("Error processing live audio transcription callback data.", ex));
                }

                return errored || stopToken.IsCancellationRequested ? 1 : 0;
            };

            _session.SetStreamingCallback(streamingCallback);

            var request = CreateRequest(
                language,
                static () => new Request(),
                static (request, options) => request.SetOptions(options));
            _request = request;

            request.AddItem(formatDescriptor); // transfers ownership

            // Add queue without taking ownership — we still need to push items into it
            Api.CheckStatus(Api.Inference.RequestAddItem(request.Ptr, _queue.Ptr, false));

            _processingTask = RunProducerAsync(() =>
            {
                var responsePtr = _session.ProcessRequest(request.Ptr);

                // Drain the final Response: it carries the aggregated transcription as a SpeechResultItem
                using (var response = new Response(responsePtr))
                {
                    var finalText = new System.Text.StringBuilder();
                    foreach (var responseItem in response)
                    {
                        using (responseItem)
                        {
                            if (responseItem is SpeechResultItem resultItem &&
                                !string.IsNullOrEmpty(resultItem.Text))
                            {
                                finalText.Append(resultItem.Text);
                            }
                        }
                    }

                    if (finalText.Length > 0)
                    {
                        var finalTextStr = finalText.ToString();
                        channel.Writer.TryWrite(new LiveAudioTranscriptionResponse
                        {
                            IsFinal = true,
                            Content =
                            [
                                new ContentPart
                                {
                                    Text = finalTextStr,
                                    Transcript = finalTextStr
                                }
                            ]
                        });
                    }
                }
            }, error =>
            {
                if (error != null)
                {
                    channel.Writer.TryComplete(
                        new FoundryLocalException("Error during live audio transcription processing.", error));
                }
                else
                {
                    channel.Writer.TryComplete();
                }
            }, stopToken, () => _model.AcquireManagerLease(trackReentrancy: true));

            _state = SessionState.Started;
            return Task.CompletedTask;
        }
        catch
        {
            formatDescriptor.Dispose();
            _state = SessionState.Disposed;
            Cleanup();
            throw;
        }
    }

    internal static TRequest CreateRequest<TRequest>(
        string? language,
        Func<TRequest> requestFactory,
        Action<TRequest, RequestOptions> setOptions)
        where TRequest : IDisposable
    {
        var request = requestFactory();

        try
        {
            if (language != null)
            {
                setOptions(
                    request,
                    new RequestOptions
                    {
                        AdditionalOptions = new Dictionary<string, string>
                        {
                            ["language"] = language
                        }
                    });
            }

            return request;
        }
        catch
        {
            request.Dispose();
            throw;
        }
    }

    public ValueTask AppendAsync(ReadOnlyMemory<byte> pcmData, CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();

        lock (_stateSync)
        {
            Detail.Throw.IfDisposed(_state == SessionState.Disposed, this);

            if (_state != SessionState.Started)
            {
                throw new FoundryLocalException($"Session must be Started to append audio (was {_state}).");
            }

            var bytesItem = BytesItem.CreateOwned(pcmData);
            try
            {
                _queue!.Push(bytesItem); // transfers ownership on success
                bytesItem = null!;
            }
            finally
            {
                bytesItem?.Dispose();
            }
        }

        return default;
    }

    public async IAsyncEnumerable<LiveAudioTranscriptionResponse> GetStream(
        [EnumeratorCancellation] CancellationToken ct = default)
    {
        Channel<LiveAudioTranscriptionResponse> channel;
        lock (_stateSync)
        {
            Detail.Throw.IfDisposed(_state == SessionState.Disposed, this);

            if (_state == SessionState.Created)
            {
                throw new FoundryLocalException($"Session must be Started to read stream (was {_state}).");
            }

            channel = _channel!;
        }

        await foreach (var item in channel.Reader.ReadAllAsync(ct).ConfigureAwait(false))
        {
            yield return item;
        }
    }

    public async Task StopAsync(CancellationToken ct = default)
    {
        Task? processingTask;
        lock (_stateSync)
        {
            Detail.Throw.IfDisposed(_state == SessionState.Disposed, this);

            if (_state != SessionState.Started)
            {
                return;
            }

            _state = SessionState.Stopped;
            _queue!.MarkFinished();
            processingTask = _processingTask;
        }

        if (processingTask != null)
        {
            await WaitWithCancellationAsync(processingTask, ct).ConfigureAwait(false);
        }
    }

    private static async Task WaitWithCancellationAsync(Task task, CancellationToken ct)
    {
        if (!ct.CanBeCanceled)
        {
            await task.ConfigureAwait(false);
            return;
        }

        var cancellation = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        using var registration = ct.Register(() => cancellation.TrySetResult(true));
        if (await Task.WhenAny(task, cancellation.Task).ConfigureAwait(false) != task)
        {
            ct.ThrowIfCancellationRequested();
        }

        await task.ConfigureAwait(false);
    }

    public async ValueTask DisposeAsync()
    {
        Task? processingTask;
        lock (_stateSync)
        {
            if (_state == SessionState.Disposed)
            {
                return;
            }

            var wasStarted = _state == SessionState.Started;
            _state = SessionState.Disposed;
            if (wasStarted)
            {
                _queue?.MarkFinished();
                try { _stopCts?.Cancel(); } catch { }
            }

            processingTask = _processingTask;
        }

        if (processingTask != null)
        {
            try
            {
                await processingTask.ConfigureAwait(false);
            }
            catch
            {
                // Best-effort cleanup during dispose
            }
        }

        Cleanup();
        GC.SuppressFinalize(this);
    }

    private void Cleanup()
    {
        if (Interlocked.Exchange(ref _cleanupStarted, 1) != 0)
        {
            return;
        }

        TryDispose(_request);
        TryDispose(_queue);
        TryDispose(_session);
        TryDispose(_managerRegistration);
        TryDispose(_managerLease);
        _managerLease = null;
        TryDispose(_stopCts);
    }

    private static void TryDispose(IDisposable? resource)
    {
#pragma warning disable IDISP007 // All callers pass resources owned by this session or producer invocation.
        try { resource?.Dispose(); } catch { }
#pragma warning restore IDISP007
    }

    internal static Task RunProducerAsync(Action process, Action<Exception?> complete, CancellationToken token,
                                          Func<IDisposable>? acquireLifetime = null)
    {
        return Task.Run(() =>
        {
            Exception? error = null;
            IDisposable? lifetime = null;
            try
            {
                lifetime = acquireLifetime?.Invoke();
                token.ThrowIfCancellationRequested();
                process();
            }
            catch (OperationCanceledException)
            {
            }
            catch (Exception ex)
            {
                error = ex;
            }
            finally
            {
                try
                {
                    complete(error);
                }
                finally
                {
                    TryDispose(lifetime);
                }
            }
        }, CancellationToken.None);
    }

    ~LiveAudioTranscriptionSession()
    {
#pragma warning disable IDISP023 // Finalization must release native dependents before their manager lease.
        Cleanup();
#pragma warning restore IDISP023
    }

    private sealed class SessionRegistrationTarget : IDisposable
    {
        private LiveAudioTranscriptionSession? _owner;

        internal SessionRegistrationTarget(LiveAudioTranscriptionSession owner)
        {
            _owner = owner;
        }

        public void Dispose()
        {
            Interlocked.Exchange(ref _owner, null)?.DisposeAsync().AsTask().GetAwaiter().GetResult();
        }
    }
}
