// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Threading.Channels;

using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.AI.Foundry.Local.Detail.Native;

using NativeSession = Microsoft.AI.Foundry.Local.Detail.Native.Session;

/// <summary>
/// Base session wrapping the native inference session.
/// Provides request processing, streaming, options, and tool definitions.
/// Use <see cref="ChatSession"/> or <see cref="AudioSession"/> for task-specific validation.
/// </summary>
public abstract class Session : IDisposable
{
    private readonly NativeSession _session;
    private FlStreamingCallback? _nativeStreamingCallback;
    private Channel<Item>? _activeChannel;
    private CancellationToken _streamingCt;
    private Task? _activeStreamingTask;
    private CancellationTokenSource? _activeStreamingCts;
    private bool _disposed;

    /// <summary>
    /// Create a session from a loaded model. Subclasses should validate the model task before calling this.
    /// </summary>
    protected Session(IModel model)
    {
        var concrete = (Model)model;
        _session = new NativeSession(concrete.NativeModel);
    }

    /// <summary>
    /// Set session-level inference options. These apply to all subsequent
    /// <see cref="ProcessRequestAsync"/> calls unless overridden per-request.
    /// </summary>
    /// <returns>This session (fluent).</returns>
    public Session SetOptions(RequestOptions options)
    {
        ThrowIfDisposed();

        Detail.Throw.IfNull(options);

        Api.Root.CreateKeyValuePairs(out var kvpPtr);

        try
        {
            foreach (var kvp in options.ToDictionary())
            {
                Api.Root.AddKeyValuePair(kvpPtr, kvp.Key, kvp.Value);
            }

            _session.SetOptions(kvpPtr);
        }
        finally
        {
            Api.Root.KeyValuePairsRelease(kvpPtr);
        }

        return this;
    }

    /// <summary>
    /// Enable or disable streaming mode. When enabled, a native streaming callback is installed
    /// on the session. Use <see cref="ProcessStreamingRequestAsync"/> to receive items as they are generated.
    /// The callback remains installed until disabled or the session is disposed.
    /// </summary>
    /// <returns>This session (fluent).</returns>
    public Session SetStreaming(bool enabled)
    {
        ThrowIfDisposed();

        if (enabled && _nativeStreamingCallback == null)
        {
            _nativeStreamingCallback = (FlStreamingCallbackData data, IntPtr userData) =>
            {
                var channel = _activeChannel;
                if (channel == null)
                {
                    return 0;
                }

                bool errored = false;

                try
                {
                    if (data.ItemQueue != IntPtr.Zero)
                    {
                        while (Api.Item.QueueTryPop(data.ItemQueue, out var itemPtr))
                        {
                            // Ownership transfers to the channel consumer who disposes it
#pragma warning disable IDISP001
                            var item = Item.FromNative(itemPtr, ownsHandle: true);
#pragma warning restore IDISP001
                            if (!channel.Writer.TryWrite(item))
                            {
                                item.Dispose();
                            }
                        }
                    }
                }
                catch (Exception ex)
                {
                    errored = true;
                    channel.Writer.TryComplete(
                        new FoundryLocalException("Error processing streaming callback data.", ex));
                }

                return errored || _streamingCt.IsCancellationRequested ? 1 : 0;
            };

            _session.SetStreamingCallback(_nativeStreamingCallback);
        }
        else if (!enabled && _nativeStreamingCallback != null)
        {
            _session.SetStreamingCallback(null);
            _nativeStreamingCallback = null;
        }

        return this;
    }

    /// <summary>
    /// Process a request and return the complete response.
    /// </summary>
    public async Task<Response> ProcessRequestAsync(Request request, CancellationToken ct = default)
    {
        ThrowIfDisposed();

        return await Task.Run(() =>
        {
            var responsePtr = _session.ProcessRequest(request.Ptr);
            return new Response(responsePtr);
        }, ct).ConfigureAwait(false);
    }

    /// <summary>
    /// Process a request with streaming. Returns a <see cref="StreamingResponse"/> whose async
    /// iterator yields <see cref="Item"/>s as they are produced and whose
    /// <see cref="StreamingResponse.FinalResponse"/> resolves to the terminal
    /// <see cref="Response"/> (carrying <see cref="FinishReason"/>, usage, and any aggregated
    /// items) after the iterator drains.
    ///
    /// Requires <see cref="SetStreaming"/> to have been called with <c>true</c>.
    /// Concurrent streaming requests on the same session are not supported.
    ///
    /// The caller MUST either await <see cref="StreamingResponse.FinalResponse"/> (and dispose the
    /// returned <see cref="Response"/>) or <c>await using</c> the <see cref="StreamingResponse"/>
    /// to avoid leaking the native response handle.
    /// </summary>
    /// <exception cref="InvalidOperationException">
    /// Thrown if streaming has not been enabled via <see cref="SetStreaming"/>, or if another
    /// streaming request is already in flight on this session (concurrent streaming requests
    /// on the same session are not supported).
    /// </exception>
    public StreamingResponse ProcessStreamingRequestAsync(Request request, CancellationToken ct = default)
    {
        ThrowIfDisposed();

        Detail.Throw.IfNull(request);

        if (_nativeStreamingCallback == null)
        {
            throw new InvalidOperationException(
                "Streaming not enabled. Call SetStreaming(true) before ProcessStreamingRequestAsync.");
        }

        var channel = Channel.CreateUnbounded<Item>(
            new UnboundedChannelOptions
            {
                SingleWriter = true,
                SingleReader = true,
                AllowSynchronousContinuations = true,
            });

        if (Interlocked.CompareExchange(ref _activeChannel, channel, null) != null)
        {
            throw new InvalidOperationException(
                "Concurrent streaming requests on the same session are not supported. "
                + "Drain or cancel the in-flight stream before starting another.");
        }

        var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        _streamingCt = cts.Token;
#pragma warning disable IDISP003 // Ownership transferred to the returned StreamingResponse, which disposes the cts.
        _activeStreamingCts = cts;
#pragma warning restore IDISP003

        var tcs = new TaskCompletionSource<Response>(TaskCreationOptions.RunContinuationsAsynchronously);

        var task = Task.Run(() =>
        {
            IntPtr responsePtr;
            bool wasCancelledBeforeReturn;

            try
            {
                responsePtr = _session.ProcessRequest(request.Ptr);

                // Capture the cancellation state BEFORE completing the channel. Channel completion
                // (with AllowSynchronousContinuations = true) can synchronously run the consumer's
                // await-foreach finally, which calls cts.Cancel() — that would otherwise make this
                // check observe cancellation even when the stream drained naturally.
                wasCancelledBeforeReturn = cts.IsCancellationRequested;
            }
            catch (OperationCanceledException)
            {
                channel.Writer.TryComplete();
                tcs.TrySetCanceled(cts.Token);
                Interlocked.Exchange(ref _activeChannel, null);
                return;
            }
            catch (Exception ex)
            {
                var wrapped = new FoundryLocalException("Error executing streaming request.", ex);
                channel.Writer.TryComplete(wrapped);
                tcs.TrySetException(wrapped);
                Interlocked.Exchange(ref _activeChannel, null);
                return;
            }

            // Complete the channel before publishing FinalResponse so any consumer awaiting both
            // observes iterator completion strictly before FinalResponse settles.
            channel.Writer.TryComplete();

            if (wasCancelledBeforeReturn)
            {
                // Cancelled mid-stream — drop the (potentially partial / undefined) native response.
                Api.Inference.ResponseRelease(responsePtr);
                tcs.TrySetCanceled(cts.Token);
            }
            else
            {
#pragma warning disable IDISP004 // Ownership transferred to FinalResponse consumer (or DisposeAsync).
                tcs.TrySetResult(new Response(responsePtr));
#pragma warning restore IDISP004
            }

            Interlocked.Exchange(ref _activeChannel, null);
        }, CancellationToken.None);

        _activeStreamingTask = task;

        return new StreamingResponse(this, channel, cts, task, tcs);
    }

    internal void ClearStreamingState()
    {
        _activeStreamingTask = null;
#pragma warning disable IDISP003 // cts is disposed by the owning StreamingResponse; we just clear the field reference.
        _activeStreamingCts = null;
#pragma warning restore IDISP003
    }

    public void Dispose()
    {
        Dispose(true);
        GC.SuppressFinalize(this);
    }

    protected virtual void Dispose(bool disposing)
    {
        if (!_disposed)
        {
            if (disposing)
            {
                // If a streaming enumeration is active, signal cancellation and wait for the
                // producer task to complete before tearing down the native session. This prevents
                // a use-after-free when Dispose() races with an in-flight ProcessStreamingRequestAsync.
                try { _activeStreamingCts?.Cancel(); } catch { }

                var streamingTask = _activeStreamingTask;
                if (streamingTask != null)
                {
                    try
                    {
                        streamingTask.Wait(TimeSpan.FromSeconds(30));
                    }
                    catch
                    {
                        // Swallow — we're tearing down regardless.
                    }
                }

                _session.Dispose();
            }

            _disposed = true;
        }
    }

    protected NativeSession GetNativeSession() { return _session; }

    protected void ThrowIfDisposed()
    {
        Detail.Throw.IfDisposed(_disposed, this);
    }
}
