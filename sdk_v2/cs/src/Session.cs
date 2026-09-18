// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Threading.Channels;

using Microsoft.AI.Foundry.Local.Detail;
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
    private readonly NativeSession _session = null!;
    private readonly ManagerLifetime _managerLifetime = null!;
    private readonly ManagerLifetime.SessionRegistration _managerRegistration = null!;
    private readonly SessionOperationGate _operationGate = new();
    private readonly OwnershipSlot<StreamingOperation> _streamSlot = new();
    private FlStreamingCallback? _nativeStreamingCallback;
    private int _disposed;

    /// <summary>
    /// Create a session from a loaded model. Subclasses should validate the model task before calling this.
    /// </summary>
    protected Session(IModel model)
    {
        var concrete = (Model)model;
        using var managerLease = concrete.AcquireManagerLease(trackReentrancy: true);
        try
        {
            _session = new NativeSession(concrete.NativeModel);
            _managerLifetime = concrete.NativeLifetime;
            _managerRegistration = concrete.NativeLifetime.RegisterSession(this);
        }
        catch
        {
            _session?.Dispose();
            throw;
        }
    }

    /// <summary>
    /// Set session-level inference options. These apply to all subsequent
    /// <see cref="ProcessRequestAsync"/> calls unless overridden per-request.
    /// </summary>
    /// <returns>This session (fluent).</returns>
    public Session SetOptions(RequestOptions options)
    {
        Detail.Throw.IfNull(options);

        using var operation = _operationGate.Acquire(this);
        using var managerLease = _managerLifetime.Acquire(this, trackReentrancy: true);

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
        using var operation = _operationGate.Acquire(this);
        using var managerLease = _managerLifetime.Acquire(this, trackReentrancy: true);

        if (enabled && _nativeStreamingCallback == null)
        {
            _nativeStreamingCallback = (FlStreamingCallbackData data, IntPtr userData) =>
            {
                var stream = _streamSlot.Value;

                if (stream == null)
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
                            if (!stream.Channel.Writer.TryWrite(item))
                            {
                                item.Dispose();
                            }
                        }
                    }
                }
                catch (Exception ex)
                {
                    errored = true;
                    stream.Channel.Writer.TryComplete(
                        new FoundryLocalException("Error processing streaming callback data.", ex));
                }

                return errored || stream.Cts.IsCancellationRequested ? 1 : 0;
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
        Detail.Throw.IfNull(request);
        var operation = _operationGate.Acquire(this, assignCurrentThread: false);

        return await Task.Run(() =>
        {
            operation.SetCurrentThread();
            try
            {
                ct.ThrowIfCancellationRequested();
                using var managerLease = _managerLifetime.Acquire(this, trackReentrancy: true);
                var responsePtr = _session.ProcessRequest(request.Ptr);
                return new Response(responsePtr);
            }
            finally
            {
                operation.ClearCurrentThread();
                operation.Dispose();
            }
        }, CancellationToken.None).ConfigureAwait(false);
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
        Detail.Throw.IfNull(request);

#pragma warning disable IDISP001 // Ownership transfers to StreamingOperation and then StreamingResponse.
#pragma warning disable IDISP016 // The operation is disposed only on the throwing precondition branch.
    var operation = _operationGate.TryAcquire(this, assignCurrentThread: false);
    if (operation == null)
    {
        throw new InvalidOperationException("Concurrent streaming requests on the same session are not supported.");
    }

        if (_nativeStreamingCallback == null)
        {
            operation.Dispose();
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

        var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        var stream = new StreamingOperation(channel, cts, operation);
        if (!_streamSlot.TrySet(stream))
        {
            stream.Dispose();
            throw new InvalidOperationException("A streaming response still owns this session.");
        }

        var tcs = new TaskCompletionSource<Response>(TaskCreationOptions.RunContinuationsAsynchronously);

        _ = Task.Run(() =>
        {
            operation.SetCurrentThread();
            try
            {
                try
                {
                    using var managerLease = _managerLifetime.Acquire(this, trackReentrancy: true);
                    var responsePtr = _session.ProcessRequest(request.Ptr);

                    // Capture cancellation before channel completion, which can synchronously run cleanup.
                    var wasCancelledBeforeReturn = cts.IsCancellationRequested;

                    channel.Writer.TryComplete();

                    if (wasCancelledBeforeReturn)
                    {
                        Api.Inference.ResponseRelease(responsePtr);
                        tcs.TrySetCanceled(cts.Token);
                    }
                    else
                    {
#pragma warning disable IDISP004 // Ownership transferred to FinalResponse consumer (or DisposeAsync).
                        tcs.TrySetResult(new Response(responsePtr));
#pragma warning restore IDISP004
                    }
                }
                catch (OperationCanceledException)
                {
                    channel.Writer.TryComplete();
                    tcs.TrySetCanceled(cts.Token);
                    return;
                }
                catch (Exception ex)
                {
                    var wrapped = new FoundryLocalException("Error executing streaming request.", ex);
                    channel.Writer.TryComplete(wrapped);
                    tcs.TrySetException(wrapped);
                    return;
                }
            }
            finally
            {
                operation.ClearCurrentThread();
                stream.MarkProducerCompleted();
                if (_operationGate.IsClosed)
                {
                    ReleaseStreamingOperation(stream);
                }
            }
        }, CancellationToken.None);

        if (_operationGate.IsClosed)
        {
            cts.Cancel();
        }

        return new StreamingResponse(this, stream, tcs);
    #pragma warning restore IDISP016
    #pragma warning restore IDISP001
    }

    internal void ReleaseStreamingOperation(StreamingOperation stream)
    {
        if (_streamSlot.ClearIfOwned(stream))
        {
            stream.Release();
        }
    }

    public void Dispose()
    {
        Dispose(true);
        GC.SuppressFinalize(this);
    }

    ~Session()
    {
        try
        {
            Dispose(false);
        }
        catch
        {
        }
    }

    protected virtual void Dispose(bool disposing)
    {
#pragma warning disable IDISP023 // Finalization must close operations and release session before manager registration.
        if (!_operationGate.BeginClose())
        {
            return;
        }

        Interlocked.Exchange(ref _disposed, 1);

        try
        {
            var stream = _streamSlot.Value;

            stream?.Cancel();
            stream?.WaitForProducer();
            if (stream != null)
            {
                ReleaseStreamingOperation(stream);
            }
        }
        finally
        {
            try
            {
                _operationGate.WaitForOperations();
            }
            finally
            {
                try
                {
                    _session?.Dispose();
                }
                finally
                {
                    _managerRegistration?.Dispose();
                }
            }
        }
#pragma warning restore IDISP023
    }

    protected T ExecuteNative<T>(Func<NativeSession, T> operation)
    {
        using var admission = _operationGate.Acquire(this);
        using var managerLease = _managerLifetime.Acquire(this, trackReentrancy: true);
        return operation(_session);
    }

    protected void ExecuteNative(Action<NativeSession> operation)
    {
        using var admission = _operationGate.Acquire(this);
        using var managerLease = _managerLifetime.Acquire(this, trackReentrancy: true);
        operation(_session);
    }

    protected void ThrowIfDisposed()
    {
        Detail.Throw.IfDisposed(Volatile.Read(ref _disposed) != 0, this);
    }

    internal sealed class StreamingOperation : IDisposable
    {
        private readonly SessionOperationGate.Operation _operation;
        private readonly TaskCompletionSource<bool> _producerCompleted =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        private int _released;

        internal StreamingOperation(Channel<Item> channel, CancellationTokenSource cts,
                                    SessionOperationGate.Operation operation)
        {
            Channel = channel;
            Cts = cts;
            _operation = operation;
        }

        internal Channel<Item> Channel { get; }
        internal CancellationTokenSource Cts { get; }
        internal Task ProducerTask => _producerCompleted.Task;
        internal void MarkProducerCompleted() => _producerCompleted.TrySetResult(true);
        internal void WaitForProducer() => _producerCompleted.Task.GetAwaiter().GetResult();
        internal void Cancel()
        {
            try { Cts.Cancel(); } catch { }
        }

        internal void Release()
        {
            if (Interlocked.Exchange(ref _released, 1) == 0)
            {
#pragma warning disable IDISP007 // Ownership transferred into StreamingOperation by its constructor.
                _operation.Dispose();
#pragma warning restore IDISP007
            }
        }

        public void Dispose() => Release();
    }
}
