// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Detail;

internal sealed class SessionOperationGate
{
    private readonly object _sync = new();
    private bool _closed;
    private bool _active;
    private int _activeThreadId;
    private TaskCompletionSource<bool> _changed = new(TaskCreationOptions.RunContinuationsAsynchronously);

    internal bool IsClosed
    {
        get
        {
            lock (_sync)
            {
                return _closed;
            }
        }
    }

    internal Operation Acquire(object owner, bool assignCurrentThread = true)
    {
        lock (_sync)
        {
            Throw.IfDisposed(_closed, owner);
            while (_active)
            {
                Monitor.Wait(_sync);
                Throw.IfDisposed(_closed, owner);
            }

            _active = true;
            _activeThreadId = assignCurrentThread ? Environment.CurrentManagedThreadId : 0;
            return new Operation(this);
        }
    }

    internal async Task<Operation> AcquireAsync(object owner, CancellationToken ct)
    {
        while (true)
        {
            Task changed;
            lock (_sync)
            {
                Throw.IfDisposed(_closed, owner);
                ct.ThrowIfCancellationRequested();
                if (!_active)
                {
                    _active = true;
                    _activeThreadId = 0;
                    return new Operation(this);
                }

                changed = _changed.Task;
            }

            if (!ct.CanBeCanceled)
            {
                await changed.ConfigureAwait(false);
                continue;
            }

            var canceled = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            using var registration = ct.Register(() => canceled.TrySetCanceled(ct));
            await (await Task.WhenAny(changed, canceled.Task).ConfigureAwait(false)).ConfigureAwait(false);
        }
    }

    internal Operation? TryAcquire(object owner, bool assignCurrentThread = true)
    {
        lock (_sync)
        {
            Throw.IfDisposed(_closed, owner);
            if (_active)
            {
                return null;
            }

            _active = true;
            _activeThreadId = assignCurrentThread ? Environment.CurrentManagedThreadId : 0;
            return new Operation(this);
        }
    }

    internal bool BeginClose()
    {
        lock (_sync)
        {
            if (_closed)
            {
                return false;
            }

            if (_activeThreadId == Environment.CurrentManagedThreadId)
            {
                throw new InvalidOperationException("Cannot dispose a Session during one of its active native calls.");
            }

            _closed = true;
            Monitor.PulseAll(_sync);
            _changed.TrySetResult(true);
            return true;
        }
    }

    internal void WaitForOperations()
    {
        lock (_sync)
        {
            while (_active)
            {
                Monitor.Wait(_sync);
            }
        }
    }

    private void SetCurrentThread()
    {
        lock (_sync)
        {
            _activeThreadId = Environment.CurrentManagedThreadId;
        }
    }

    private void ClearCurrentThread()
    {
        lock (_sync)
        {
            if (_activeThreadId == Environment.CurrentManagedThreadId)
            {
                _activeThreadId = 0;
            }
        }
    }

    private void Release()
    {
        lock (_sync)
        {
            _active = false;
            _activeThreadId = 0;
            Monitor.PulseAll(_sync);
            var changed = _changed;
            _changed = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            changed.TrySetResult(true);
        }
    }

    internal sealed class Operation : IDisposable
    {
        private SessionOperationGate? _owner;

        internal Operation(SessionOperationGate owner)
        {
            _owner = owner;
        }

        internal void SetCurrentThread() => _owner?.SetCurrentThread();

        internal void ClearCurrentThread() => _owner?.ClearCurrentThread();

        public void Dispose()
        {
            Interlocked.Exchange(ref _owner, null)?.Release();
        }
    }
}

internal sealed class OwnershipSlot<T> where T : class
{
    private readonly object _sync = new();
    private T? _value;

    internal T? Value
    {
        get
        {
            lock (_sync)
            {
                return _value;
            }
        }
    }

    internal bool TrySet(T value)
    {
        lock (_sync)
        {
            if (_value != null)
            {
                return false;
            }

            _value = value;
            return true;
        }
    }

    internal bool ClearIfOwned(T value)
    {
        lock (_sync)
        {
            if (!ReferenceEquals(_value, value))
            {
                return false;
            }

            _value = null;
            return true;
        }
    }
}
