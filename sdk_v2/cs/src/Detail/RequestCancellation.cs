// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Detail;

using System.Collections.Concurrent;
using System.Runtime.ExceptionServices;

internal sealed class RequestCancellation : IDisposable
{
    private static readonly Lazy<Dispatcher> SharedDispatcher = new(() => new Dispatcher());
    private readonly object _sync = new();
    private readonly Action _cancel;
    private readonly CancellationTokenRegistration _registration;
    private ExceptionDispatchInfo? _error;
    private int _disposed;

    internal RequestCancellation(Action cancel, CancellationToken ct)
    {
        _cancel = cancel;
        _registration = ct.Register(() =>
        {
            try
            {
                if (Retry())
                {
                    SharedDispatcher.Value.Enqueue(this);
                }
            }
            catch (Exception ex)
            {
                lock (_sync)
                {
                    _error = ExceptionDispatchInfo.Capture(ex);
                }
            }
        });
    }

    private bool Retry()
    {
        lock (_sync)
        {
            if (Volatile.Read(ref _disposed) != 0 || _error != null)
            {
                return false;
            }

            try
            {
                _cancel();
                return true;
            }
            catch (Exception ex)
            {
                _error = ExceptionDispatchInfo.Capture(ex);
                return false;
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
        if (SharedDispatcher.IsValueCreated)
        {
            SharedDispatcher.Value.Wake();
        }

        // Taking the same lock as Retry joins any active cancel call before the request lease is released.
        lock (_sync)
        {
            _error?.Throw();
        }
    }

#pragma warning disable CA1001, IDISP006 // The shared worker and its wake event live for the process lifetime.
    private sealed class Dispatcher
    {
        private readonly ConcurrentQueue<RequestCancellation> _queued = new();
        private readonly AutoResetEvent _wake = new(false);

        internal Dispatcher()
        {
            new Thread(Run) { IsBackground = true, Name = "FoundryLocalCancellation" }.Start();
        }

        internal void Enqueue(RequestCancellation cancellation)
        {
            _queued.Enqueue(cancellation);
            Wake();
        }

        internal void Wake() => _wake.Set();

        private void Run()
        {
            var pending = new List<RequestCancellation>();
            while (true)
            {
                _wake.WaitOne(pending.Count == 0 ? Timeout.Infinite : 10);
                while (_queued.TryDequeue(out var cancellation))
                {
                    pending.Add(cancellation);
                }

                pending.RemoveAll(cancellation => !cancellation.Retry());
            }
        }
    }
#pragma warning restore CA1001, IDISP006
}
