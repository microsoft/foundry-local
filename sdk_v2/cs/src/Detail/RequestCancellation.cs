// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Detail;

internal sealed class RequestCancellation : IDisposable
{
    private readonly CancellationTokenSource _finished = new();
    private readonly CancellationTokenRegistration _registration;
    private Task? _cancelTask;
    private int _disposed;

    internal RequestCancellation(Action cancel, CancellationToken ct)
    {
        _registration = ct.Register(() => _cancelTask = CancelUntilFinishedAsync(cancel));
    }

    private async Task CancelUntilFinishedAsync(Action cancel)
    {
        // Native cancellation is an idle no-op. Keep retrying across admission, but never outlive the request lease.
        while (!_finished.IsCancellationRequested)
        {
            cancel();
            try
            {
                await Task.Delay(10, _finished.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (_finished.IsCancellationRequested)
            {
                break;
            }
        }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
        {
            return;
        }

        _registration.Dispose();
        _finished.Cancel();
        try
        {
            _cancelTask?.GetAwaiter().GetResult();
        }
        finally
        {
            _finished.Dispose();
        }
    }
}
