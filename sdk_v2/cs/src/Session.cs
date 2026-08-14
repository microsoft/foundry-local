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
    private CancellationTokenSource? _activeStreamingCts;
    private bool _disposed;

    // Counts native calls so Dispose can wait before releasing the session.
    private readonly object _gate = new();
    private int _activeCalls;
    private bool _disposing;

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
    /// <remarks>
    /// <paramref name="ct"/> cancels the active native request. Cancellation is retried because
    /// native cancellation does nothing before processing starts.
    /// </remarks>
    public async Task<Response> ProcessRequestAsync(Request request, CancellationToken ct = default)
    {
        ThrowIfDisposed();
        Detail.Throw.IfNull(request);
        ct.ThrowIfCancellationRequested();

        EnterActiveCall();

        IntPtr requestPtr;

        try
        {
            requestPtr = request.AcquireForProcessing();
        }
        catch
        {
            ExitActiveCall();
            throw;
        }

        return await Task.Run(() =>
        {
            RequestCancellationState? cancellationState = null;
            CancellationTokenRegistration registration = default;

            try
            {
                cancellationState = new RequestCancellationState(request);
                registration = ct.Register(static state =>
                {
                    ((RequestCancellationState)state!).Cancel();
                }, cancellationState);

                ct.ThrowIfCancellationRequested();

                IntPtr responsePtr;

                try
                {
                    responsePtr = _session.ProcessRequest(requestPtr);
                }
                catch (Exception ex) when (ct.IsCancellationRequested)
                {
                    throw new OperationCanceledException("The request was cancelled.", ex, ct);
                }

                if (ct.IsCancellationRequested)
                {
                    if (responsePtr != IntPtr.Zero)
                    {
                        Api.Inference.ResponseRelease(responsePtr);
                    }

                    throw new OperationCanceledException("The request was cancelled.", ct);
                }

                return new Response(responsePtr);
            }
            finally
            {
                cancellationState?.Complete();

                try
                {
                    request.ReleaseAfterProcessing();
                }
                finally
                {
                    ExitActiveCall();

                    // Decrement active counts before waiting for callbacks that may call Dispose.
                    registration.Dispose();
                }
            }
        }, CancellationToken.None).ConfigureAwait(false);
    }

    /// <summary>
    /// Permanently cancel this session.
    /// </summary>
    /// <remarks>
    /// Active and queued requests are cancelled. Later requests fail with invalid usage.
    /// Thread-safe and idempotent.
    /// </remarks>
    public void Cancel()
    {
        EnterActiveCall();

        try
        {
            _session.Cancel();
        }
        finally
        {
            ExitActiveCall();
        }
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

        EnterActiveCall();

        IntPtr requestPtr;

        try
        {
            requestPtr = request.AcquireForProcessing();
        }
        catch
        {
            ExitActiveCall();
            throw;
        }

        Channel<Item> channel;

        try
        {
            channel = Channel.CreateUnbounded<Item>(
                new UnboundedChannelOptions
                {
                    SingleWriter = true,
                    SingleReader = true,
                    AllowSynchronousContinuations = false,
                });

            if (Interlocked.CompareExchange(ref _activeChannel, channel, null) != null)
            {
                throw new InvalidOperationException(
                    "Concurrent streaming requests on the same session are not supported. "
                    + "Drain or cancel the in-flight stream before starting another.");
            }
        }
        catch
        {
            request.ReleaseAfterProcessing();
            ExitActiveCall();
            throw;
        }

        var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        _streamingCt = cts.Token;
#pragma warning disable IDISP003 // Ownership transferred to the returned StreamingResponse.
        _activeStreamingCts = cts;
#pragma warning restore IDISP003

        var tcs = new TaskCompletionSource<Response>(TaskCreationOptions.RunContinuationsAsynchronously);

        var task = Task.Run(() =>
        {
            RequestCancellationState? cancellationState = null;
            CancellationTokenRegistration registration = default;

            try
            {
                cancellationState = new RequestCancellationState(request);
                registration = cts.Token.Register(static state =>
                {
                    ((RequestCancellationState)state!).Cancel();
                }, cancellationState);

                IntPtr responsePtr;
                bool wasCancelledBeforeReturn;

                try
                {
                    cts.Token.ThrowIfCancellationRequested();
                    responsePtr = _session.ProcessRequest(requestPtr);

                    // Read cancellation before the consumer handles completion and cancels cts.
                    wasCancelledBeforeReturn = cts.IsCancellationRequested;
                }
                catch (Exception) when (cts.IsCancellationRequested)
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

                channel.Writer.TryComplete();

                if (wasCancelledBeforeReturn)
                {
                    if (responsePtr != IntPtr.Zero)
                    {
                        Api.Inference.ResponseRelease(responsePtr);
                    }

                    tcs.TrySetCanceled(cts.Token);
                }
                else
                {
#pragma warning disable IDISP004 // Ownership transferred to FinalResponse consumer (or DisposeAsync).
                    tcs.TrySetResult(new Response(responsePtr));
#pragma warning restore IDISP004
                }

                Interlocked.Exchange(ref _activeChannel, null);
            }
            finally
            {
                cancellationState?.Complete();

                try
                {
                    request.ReleaseAfterProcessing();
                }
                finally
                {
                    ExitActiveCall();
                    registration.Dispose();
                }
            }
        }, CancellationToken.None);

        return new StreamingResponse(this, channel, cts, task, tcs);
    }

    internal void ClearStreamingState()
    {
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
        if (!disposing)
        {
            return;
        }

        lock (_gate)
        {
            if (_disposing || _disposed)
            {
                while (!_disposed)
                {
                    Monitor.Wait(_gate);
                }

                return;
            }

            _disposing = true;
        }

        try
        {
            try { _activeStreamingCts?.Cancel(); } catch { }
            try { _session.Cancel(); } catch { }

            lock (_gate)
            {
                while (_activeCalls > 0)
                {
                    Monitor.Wait(_gate);
                }
            }

            _session.Dispose();
        }
        finally
        {
            lock (_gate)
            {
                _disposed = true;
                Monitor.PulseAll(_gate);
            }
        }
    }

    private void EnterActiveCall()
    {
        lock (_gate)
        {
            Detail.Throw.IfDisposed(_disposing || _disposed, this);
            _activeCalls++;
        }
    }

    private void ExitActiveCall()
    {
        lock (_gate)
        {
            _activeCalls--;

            if (_activeCalls == 0)
            {
                Monitor.PulseAll(_gate);
            }
        }
    }

    protected NativeSession GetNativeSession() { return _session; }

    protected void ThrowIfDisposed()
    {
        lock (_gate)
        {
            Detail.Throw.IfDisposed(_disposing || _disposed, this);
        }
    }

    private sealed class RequestCancellationState(Request request)
    {
        private static readonly TimeSpan RetryInterval = TimeSpan.FromMilliseconds(50);
        private readonly object _gate = new();
        private bool _completed;
        private bool _retryStarted;

        internal void Cancel()
        {
            lock (_gate)
            {
                if (_completed)
                {
                    return;
                }

                TryCancel();

                if (!_retryStarted)
                {
                    _retryStarted = true;
                    _ = RetryAsync();
                }
            }
        }

        internal void Complete()
        {
            lock (_gate)
            {
                _completed = true;
            }
        }

        private async Task RetryAsync()
        {
            while (true)
            {
                await Task.Delay(RetryInterval).ConfigureAwait(false);

                lock (_gate)
                {
                    if (_completed)
                    {
                        return;
                    }

                    TryCancel();
                }
            }
        }

        private void TryCancel()
        {
            try
            {
                request.Cancel();
            }
            catch
            {
                // Ignore this error and retry until processing completes.
            }
        }
    }
}