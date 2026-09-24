// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Threading.Channels;

/// <summary>
/// The result of <see cref="Session.ProcessStreamingRequestAsync"/>. Yields streamed
/// <see cref="Item"/>s via its async iterator and exposes the terminal <see cref="Response"/>
/// (carrying <see cref="FinishReason"/>, usage, and any non-streamed / aggregated items —
/// e.g. the final aggregated <see cref="TextItem"/> from <see cref="AudioSession"/>) through
/// <see cref="FinalResponse"/>.
///
/// <para>
/// Caller pattern:
/// <code>
/// var stream = session.ProcessStreamingRequestAsync(req, ct);
/// await foreach (var item in stream) { /* incremental */ }
/// using var final = await stream.FinalResponse; // finish_reason, usage, aggregated items
/// </code>
/// </para>
///
/// <para>
/// The caller MUST either await <see cref="FinalResponse"/> (and <see cref="Response.Dispose"/>
/// the result) or <c>await using</c> this object — otherwise the native response handle leaks.
/// </para>
/// </summary>
public sealed class StreamingResponse : IAsyncEnumerable<Item>, IAsyncDisposable
{
    private readonly Session _session;
    private readonly Session.StreamingOperation _operation;
    private readonly TaskCompletionSource<Response> _tcs;
    private int _enumerated;
    private int _finalResponseObserved;
    private int _cleanupStarted;
    private int _drainedNaturally;
    private int _disposed;

    internal StreamingResponse(Session session,
                               Session.StreamingOperation operation,
                               TaskCompletionSource<Response> tcs)
    {
        _session = session;
        _operation = operation;
        _tcs = tcs;
    }

    /// <summary>
    /// Resolves to the terminal <see cref="Response"/> after native processing completes
    /// (or the stream is cancelled / errors). If iteration has not started, awaiting this task
    /// discards buffered incremental items. Carries <see cref="FinishReason"/>, usage, and any
    /// non-streamed items.
    ///
    /// If the stream is cancelled, this task faults with <see cref="OperationCanceledException"/>.
    /// If the request errors, it faults with the same exception the iterator would throw.
    ///
    /// The caller owns the returned <see cref="Response"/> and must dispose it.
    /// </summary>
    public Task<Response> FinalResponse
    {
        get
        {
            Interlocked.Exchange(ref _finalResponseObserved, 1);
            return ObserveFinalResponseAsync();
        }
    }

    private async Task<Response> ObserveFinalResponseAsync()
    {
        try
        {
            return await _tcs.Task.ConfigureAwait(false);
        }
        finally
        {
            if (Volatile.Read(ref _enumerated) == 0)
            {
                await CleanupAsync().ConfigureAwait(false);
            }
        }
    }

    public IAsyncEnumerator<Item> GetAsyncEnumerator(CancellationToken cancellationToken = default)
    {
        if (Interlocked.Exchange(ref _enumerated, 1) != 0)
        {
            throw new InvalidOperationException("StreamingResponse can only be enumerated once.");
        }

        return EnumerateAsync(cancellationToken).GetAsyncEnumerator(cancellationToken);
    }

    // Instance shadow of TaskAsyncEnumerableExtensions.ConfigureAwait. Because this type implements both
    // IAsyncEnumerable<Item> and IAsyncDisposable, an extension-method call to .ConfigureAwait(false) is
    // ambiguous between the two overloads. Defining this instance method takes precedence over the
    // extensions and resolves to the iteration overload (which is what `await foreach` callers want).
    public ConfiguredCancelableAsyncEnumerable<Item> ConfigureAwait(bool continueOnCapturedContext)
        => ((IAsyncEnumerable<Item>)this).ConfigureAwait(continueOnCapturedContext);

    // Same disambiguation for .WithCancellation, which has no ambiguity today but would the moment any
    // similar IAsyncDisposable extension appears. Cheap to add now.
    public ConfiguredCancelableAsyncEnumerable<Item> WithCancellation(CancellationToken cancellationToken)
        => ((IAsyncEnumerable<Item>)this).WithCancellation(cancellationToken);

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
        {
            return;
        }

        await CleanupAsync().ConfigureAwait(false);

        // If FinalResponse was never observed, dispose the Response so the native handle doesn't leak.
        if (Interlocked.Exchange(ref _finalResponseObserved, 1) == 0)
        {
            if (_tcs.Task.Status == TaskStatus.RanToCompletion)
            {
                _tcs.Task.Result.Dispose();
            }
        }

    }

    private async IAsyncEnumerable<Item> EnumerateAsync([EnumeratorCancellation] CancellationToken ct = default)
    {
        using var reg = ct.Register(static s =>
        {
            try { ((CancellationTokenSource)s!).Cancel(); } catch { }
        }, _operation.Cts);

        try
        {
            await foreach (var item in _operation.Channel.Reader.ReadAllAsync(_operation.Cts.Token).ConfigureAwait(false))
            {
                yield return item;
            }

            // The channel completed on its own: the producer finished ProcessRequest and called
            // TryComplete (channel completion is always preceded by ProcessRequest returning). The
            // native request has therefore already produced its full output, so cleanup MUST NOT
            // signal abort — doing so races the native streaming-callback worker thread and can
            // transition a still-finalizing request to canceled, truncating the output. Only an early
            // break / dispose / external-token cancellation (which skip this line) should abort.
            Interlocked.Exchange(ref _drainedNaturally, 1);
        }
        finally
        {
            await CleanupAsync().ConfigureAwait(false);
        }
    }

    private async ValueTask CleanupAsync()
    {
        if (Interlocked.Exchange(ref _cleanupStarted, 1) != 0)
        {
            return;
        }

        // Signal the native callback (and producer) to stop ONLY when the consumer is abandoning
        // the stream early (break / dispose / external cancellation before the channel completed).
        // On the natural-drain path the request already finished, and cancelling here would race the
        // native callback worker thread and could truncate a still-in-flight ProcessRequest.
        if (Volatile.Read(ref _drainedNaturally) == 0)
        {
            _operation.Cancel();
        }

        // Drain and dispose any items still buffered so native handles don't leak.
        while (_operation.Channel.Reader.TryRead(out var leftover))
        {
            leftover.Dispose();
        }

        try
        {
            await _operation.ProducerTask.ConfigureAwait(false);
        }
        catch
        {
            // Producer exceptions are routed via channel completion / FinalResponse; swallow on cleanup.
        }

        while (_operation.Channel.Reader.TryRead(out var leftover))
        {
            leftover.Dispose();
        }

        _session.ReleaseStreamingOperation(_operation);
#pragma warning disable IDISP007 // Ownership transferred from Session via the StreamingResponse ctor.
        _operation.Cts.Dispose();
#pragma warning restore IDISP007
    }
}
