// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.Extensions.Logging.Abstractions;

using NativeModel = Microsoft.AI.Foundry.Local.Detail.Native.Model;

internal sealed class ManagerLifetimeTests
{
    [Test]
    public async Task DisposeSessions_IdleSessionOnSameThread_UnregistersWithoutWaiting()
    {
        using var lifetime = new ManagerLifetime();
        ManagerLifetime.SessionRegistration? registration = null;
        var session = new CallbackDisposable(() => registration!.Dispose());
        registration = lifetime.RegisterSession(session);

        await Assert.That(lifetime.TryBeginDispose()).IsTrue();
        lifetime.DisposeSessions();
        lifetime.WaitForLeases();

        await Assert.That(session.DisposeCount).IsEqualTo(1);
    }

    [Test]
    public async Task Dispose_ShutsDownThenWaitsForSessionLease_AndRejectsNewCalls()
    {
        using var lifetime = new ManagerLifetime();
        using var shutdownCalled = new ManualResetEventSlim(false);
        var sessionLease = lifetime.Acquire(this);

        var disposeTask = Task.Run(() =>
        {
            if (!lifetime.TryBeginDispose())
            {
                throw new InvalidOperationException("Disposal did not begin.");
            }

            shutdownCalled.Set();
            lifetime.WaitForLeases();
        });

        shutdownCalled.Wait();

        await Assert.That(disposeTask.IsCompleted).IsFalse();
        await Assert.That(() => lifetime.Acquire(this)).Throws<ObjectDisposedException>();

        sessionLease.Dispose();
        await disposeTask;
    }

    [Test]
    public async Task Dispose_FromOperationLease_ThrowsWithoutClosingAdmission()
    {
        using var lifetime = new ManagerLifetime();
        InvalidOperationException? caught = null;

        using (lifetime.Acquire(this, trackReentrancy: true))
        {
            try
            {
                lifetime.TryBeginDispose();
            }
            catch (InvalidOperationException ex)
            {
                caught = ex;
            }
        }

        await Assert.That(caught).IsNotNull();
        using var admittedAfterReentrantAttempt = lifetime.Acquire(this);
        admittedAfterReentrantAttempt.Dispose();

        await Assert.That(lifetime.TryBeginDispose()).IsTrue();
        lifetime.WaitForLeases();
    }

    [Test]
    public async Task SessionOperationGate_DisposeWaitsUntilAdmittedOperationCompletes()
    {
        var gate = new SessionOperationGate();
        using var operation = gate.Acquire(this, assignCurrentThread: false);
        using var closeStarted = new ManualResetEventSlim(false);

        var disposeTask = Task.Run(() =>
        {
            gate.BeginClose();
            closeStarted.Set();
            gate.WaitForOperations();
        });

        closeStarted.Wait();
        await Assert.That(disposeTask.IsCompleted).IsFalse();

        operation.Dispose();
        await disposeTask;
    }

    [Test]
    public async Task SessionOperationGate_ReentrantDisposeThrowsWithoutClosingAdmission()
    {
        var gate = new SessionOperationGate();
        InvalidOperationException? caught = null;
        using (gate.Acquire(this))
        {
            try
            {
                gate.BeginClose();
            }
            catch (InvalidOperationException ex)
            {
                caught = ex;
            }
        }

        await Assert.That(caught).IsNotNull();
        using var admitted = gate.Acquire(this);
        admitted.Dispose();
        await Assert.That(gate.BeginClose()).IsTrue();
    }

    [Test]
    public async Task OwnershipSlot_StaleCleanupDoesNotClearNewOwner()
    {
        var slot = new OwnershipSlot<object>();
        var first = new object();
        var second = new object();

        await Assert.That(slot.TrySet(first)).IsTrue();
        await Assert.That(slot.ClearIfOwned(first)).IsTrue();
        await Assert.That(slot.TrySet(second)).IsTrue();
        await Assert.That(slot.ClearIfOwned(first)).IsFalse();
        await Assert.That(slot.Value).IsSameReferenceAs(second);
    }

    [Test]
    public async Task Request_DisposeWaitsForActiveLease()
    {
        var request = new Request();
        using var lease = request.AcquireLease();
        using var disposeStarted = new ManualResetEventSlim(false);

        var disposeTask = Task.Run(() =>
        {
            disposeStarted.Set();
            request.Dispose();
        });

        disposeStarted.Wait();
        await Assert.That(disposeTask.IsCompleted).IsFalse();

        lease.Dispose();
        await disposeTask;
        await Assert.That(() => request.AcquireLease()).Throws<ObjectDisposedException>();
    }

    [Test]
    public async Task NativeRequestRunner_LeaseAcquisitionFailure_CompletesStreamWithError()
    {
        using var lifetime = new ManagerLifetime();
        await Assert.That(lifetime.TryBeginDispose()).IsTrue();

        var model = new Model(new NativeModel(IntPtr.Zero), NullLogger.Instance, lifetime);
        FoundryLocalException? caught = null;

        try
        {
            await foreach (var _ in NativeRequestRunner.RunStreamingAsync<object>(
                model,
                "{}",
                _ => new object(),
                NullLogger.Instance,
                "callback failed",
                "request failed",
                CancellationToken.None))
            {
            }
        }
        catch (FoundryLocalException ex)
        {
            caught = ex;
        }

        await Assert.That(caught).IsNotNull();
        await Assert.That(caught!.InnerException).IsTypeOf<ObjectDisposedException>();
        lifetime.WaitForLeases();
    }

    [Test]
    [Arguments(-1L, null)]
    [Arguments(0L, 0)]
    [Arguments(2147483647L, int.MaxValue)]
    [Arguments(2147483648L, null)]
    [Arguments(long.MaxValue, null)]
    public async Task FileSizeConversion_IsRangeSafe(long nativeValue, int? expected)
    {
        await Assert.That(ModelInfo.ToNullableFileSizeMb(nativeValue)).IsEqualTo(expected);
    }

    private sealed class CallbackDisposable(Action callback) : IDisposable
    {
        internal int DisposeCount { get; private set; }

        public void Dispose()
        {
            DisposeCount++;
            callback();
        }
    }
}
