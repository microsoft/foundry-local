// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Detail;

using System.Runtime.ExceptionServices;

internal sealed class RequestCancellation : IDisposable
{
    private readonly ManualResetEventSlim _finished = new();
    private readonly CancellationTokenRegistration _registration;
    private Thread? _cancelThread;
    private ExceptionDispatchInfo? _error;
    private int _disposed;

    internal RequestCancellation(Action cancel, CancellationToken ct)
    {
        _registration = ct.Register(() =>
        {
            try
            {
                cancel();
            }
            catch (Exception ex)
            {
                _error = ExceptionDispatchInfo.Capture(ex);
                return;
            }

            _cancelThread = new Thread(() => CancelUntilFinished(cancel)) { IsBackground = true };
            _cancelThread.Start();
        });
    }

    private void CancelUntilFinished(Action cancel)
    {
        // Retry idle cancellation across admission without depending on the inference thread pool.
        try
        {
            while (!_finished.Wait(10))
            {
                cancel();
            }
        }
        catch (Exception ex)
        {
            _error = ExceptionDispatchInfo.Capture(ex);
        }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
        {
            return;
        }

        _registration.Dispose();
        _finished.Set();
        try
        {
            _cancelThread?.Join();
            _error?.Throw();
        }
        finally
        {
            _finished.Dispose();
        }
    }
}
