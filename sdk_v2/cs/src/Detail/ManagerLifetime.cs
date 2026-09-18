// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Detail;

internal sealed class ManagerLifetime : IDisposable
{
    private readonly object _sync = new();
    private readonly ThreadLocal<int> _operationDepth = new(() => 0);
    private readonly HashSet<IDisposable> _sessions = new();
    private int _leaseCount;
    private bool _disposeStarted;

    internal bool IsDisposeStarted
    {
        get
        {
            lock (_sync)
            {
                return _disposeStarted;
            }
        }
    }

    internal Lease Acquire(object owner, bool trackReentrancy = false)
    {
        lock (_sync)
        {
            Throw.IfDisposed(_disposeStarted, owner);
            _leaseCount++;
        }

        if (trackReentrancy)
        {
            _operationDepth.Value++;
        }

        return new Lease(this, trackReentrancy);
    }

    internal bool TryBeginDispose()
    {
        lock (_sync)
        {
            if (_disposeStarted)
            {
                return false;
            }

            if (_operationDepth.Value != 0)
            {
                throw new InvalidOperationException("Cannot dispose FoundryLocalManager during an active native call.");
            }

            _disposeStarted = true;
            return true;
        }
    }

    internal SessionRegistration RegisterSession(IDisposable session)
    {
        lock (_sync)
        {
            Throw.IfDisposed(_disposeStarted, session);
            _sessions.Add(session);
        }

        return new SessionRegistration(this, session);
    }

    internal void DisposeSessions()
    {
        IDisposable[] sessions;
        lock (_sync)
        {
            sessions = _sessions.ToArray();
        }

        foreach (var session in sessions)
        {
            session.Dispose();
        }
    }

    internal void WaitForLeases()
    {
        lock (_sync)
        {
            while (_leaseCount != 0)
            {
                Monitor.Wait(_sync);
            }
        }
    }

    public void Dispose()
    {
        _operationDepth.Dispose();
    }

    private void Release(bool trackReentrancy)
    {
        if (trackReentrancy)
        {
            _operationDepth.Value--;
        }

        lock (_sync)
        {
            _leaseCount--;
            if (_leaseCount == 0)
            {
                Monitor.PulseAll(_sync);
            }
        }
    }

    private void UnregisterSession(IDisposable session)
    {
        lock (_sync)
        {
            _sessions.Remove(session);
        }
    }

    internal sealed class Lease : IDisposable
    {
        private ManagerLifetime? _owner;
        private readonly bool _trackReentrancy;

        internal Lease(ManagerLifetime owner, bool trackReentrancy)
        {
            _owner = owner;
            _trackReentrancy = trackReentrancy;
        }

        public void Dispose()
        {
            Interlocked.Exchange(ref _owner, null)?.Release(_trackReentrancy);
        }
    }

    internal sealed class SessionRegistration : IDisposable
    {
        private ManagerLifetime? _owner;
        private IDisposable? _session;

        internal SessionRegistration(ManagerLifetime owner, IDisposable session)
        {
            _owner = owner;
            _session = session;
        }

        public void Dispose()
        {
            var owner = Interlocked.Exchange(ref _owner, null);
            var session = Interlocked.Exchange(ref _session, null);
            if (owner != null && session != null)
            {
                owner.UnregisterSession(session);
            }
        }
    }
}