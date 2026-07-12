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
using Microsoft.AI.Foundry.Local.Detail.Interop;

using Api = Microsoft.AI.Foundry.Local.Detail.Native.Api;
using NativeModel = Microsoft.AI.Foundry.Local.Detail.Native.Model;
using NativeSession = Microsoft.AI.Foundry.Local.Detail.Native.Session;

#pragma warning disable IDISP001 // Dispose created — ownership transfers to Request/Queue
#pragma warning disable IDISP003 // Dispose previous before re-assigning — fields assigned once in StartAsync
#pragma warning disable CA2000   // Dispose objects before losing scope — ownership transfers

/// <summary>
/// Session for real-time audio streaming ASR (Automatic Speech Recognition).
/// Push PCM audio chunks via <see cref="AppendAsync"/> and consume transcription results
/// via <see cref="GetStream"/>.
/// </summary>
[System.Obsolete("LiveAudioTranscriptionSession is deprecated. Use AudioSession streaming instead. OpenAI types remain supported for the web-server path.", error: false)]
public sealed class LiveAudioTranscriptionSession : IAsyncDisposable
{
    private enum SessionState { Created, Started, Stopped, Disposed }

    private readonly string _modelId;
    private readonly NativeModel _nativeModel;

    private SessionState _state = SessionState.Created;
    private ItemQueue? _queue;
    private NativeSession? _session;
    private Request? _request;
    private Channel<LiveAudioTranscriptionResponse>? _channel;
    private Task? _processingTask;
    private CancellationTokenSource? _stopCts;

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

    internal LiveAudioTranscriptionSession(string modelId, NativeModel nativeModel)
    {
        _modelId = modelId;
        _nativeModel = nativeModel;
    }

    public Task StartAsync(CancellationToken ct = default)
    {
        Detail.Throw.IfDisposed(_state == SessionState.Disposed, this);

        if (_state != SessionState.Created)
        {
            throw new FoundryLocalException($"Session can only be started once (was {_state}).");
        }

        var formatDescriptor = AudioItem.CreateFormatDescriptor("pcm", Settings.SampleRate, Settings.Channels);

        _queue = new ItemQueue();

        _channel = Channel.CreateUnbounded<LiveAudioTranscriptionResponse>(
            new UnboundedChannelOptions
            {
                SingleWriter = true,
                SingleReader = true,
                AllowSynchronousContinuations = true
            });

        _session = new NativeSession(_nativeModel);

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

        _request = new Request();
        _request.AddItem(formatDescriptor); // transfers ownership

        // Add queue without taking ownership — we still need to push items into it
        Api.CheckStatus(Api.Inference.RequestAddItem(_request.Ptr, _queue.Ptr, false));

        _processingTask = Task.Run(() =>
        {
            try
            {
                var responsePtr = _session.ProcessRequest(_request.Ptr);

                // Drain the final Response: it carries the aggregated transcription as a SpeechResultItem
                using (var response = new Response(responsePtr))
                {
                    var finalText = new System.Text.StringBuilder();
                    foreach (var responseItem in response)
                    {
                        using (responseItem)
                        {
                            if (responseItem is SpeechResultItem resultItem && !string.IsNullOrEmpty(resultItem.Text))
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

                channel.Writer.TryComplete();
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                channel.Writer.TryComplete(
                    new FoundryLocalException("Error during live audio transcription processing.", ex));
            }
            catch (OperationCanceledException)
            {
                channel.Writer.TryComplete();
            }
        }, ct);

        _state = SessionState.Started;
        return Task.CompletedTask;
    }

    public ValueTask AppendAsync(ReadOnlyMemory<byte> pcmData, CancellationToken ct = default)
    {
        Detail.Throw.IfDisposed(_state == SessionState.Disposed, this);

        if (_state != SessionState.Started)
        {
            throw new FoundryLocalException($"Session must be Started to append audio (was {_state}).");
        }

        var bytesItem = BytesItem.CreateOwned(pcmData);
        _queue!.Push(bytesItem); // transfers ownership

        return default;
    }

    public async IAsyncEnumerable<LiveAudioTranscriptionResponse> GetStream(
        [EnumeratorCancellation] CancellationToken ct = default)
    {
        Detail.Throw.IfDisposed(_state == SessionState.Disposed, this);

        if (_state != SessionState.Started)
        {
            throw new FoundryLocalException($"Session must be Started to read stream (was {_state}).");
        }

        await foreach (var item in _channel!.Reader.ReadAllAsync(ct).ConfigureAwait(false))
        {
            yield return item;
        }
    }

    public async Task StopAsync(CancellationToken ct = default)
    {
        Detail.Throw.IfDisposed(_state == SessionState.Disposed, this);

        if (_state != SessionState.Started)
        {
            // Created (never started) or already Stopped — no-op.
            return;
        }

        // Signal end-of-input only. Do NOT cancel _stopCts here — the streaming callback
        // returns 1 (abort) when the stop token is signaled, which would tear down
        // ProcessRequest before it has drained queued audio and we'd lose the tail of
        // the transcription. Cancellation is reserved for DisposeAsync's abort path.
        _queue!.MarkFinished();

        if (_processingTask != null)
        {
            await _processingTask.ConfigureAwait(false);
        }

        _state = SessionState.Stopped;
    }

    public async ValueTask DisposeAsync()
    {
        if (_state == SessionState.Disposed)
        {
            return;
        }

        if (_state == SessionState.Started)
        {
            _queue?.MarkFinished();

            try { _stopCts?.Cancel(); } catch { }

            if (_processingTask != null)
            {
                try
                {
                    await _processingTask.ConfigureAwait(false);
                }
                catch
                {
                    // Best-effort cleanup during dispose
                }
            }
        }

        _queue?.Dispose();
        _session?.Dispose();
        _request?.Dispose();
        _stopCts?.Dispose();

        _state = SessionState.Disposed;
    }
}
