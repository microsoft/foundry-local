// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.Extensions.Logging.Abstractions;

using NativeModel = Microsoft.AI.Foundry.Local.Detail.Native.Model;
using RequestPreflightOperation = Microsoft.AI.Foundry.Local.Detail.Native.RequestPreflightOperation;

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
    [Arguments(false)]
    [Arguments(true)]
    public async Task Dispose_WaitsForLeaseRelease_AndRejectsNewCalls(bool capturedPreflight)
    {
        using var lifetime = new ManagerLifetime();
        var lease = lifetime.Acquire(this);
        using IDisposable heldResource = capturedPreflight
            ? new RequestPreflightOperation(IntPtr.Zero, lease)
            : lease;
        using var disposeStarted = new ManualResetEventSlim(false);

        var disposeTask = Task.Run(() =>
        {
            if (!lifetime.TryBeginDispose())
            {
                throw new InvalidOperationException("Disposal did not begin.");
            }

            disposeStarted.Set();
            lifetime.WaitForLeases();
        });

        try
        {
            await Assert.That(disposeStarted.Wait(TimeSpan.FromSeconds(5))).IsTrue();
            var observationWindow = Task.Delay(TimeSpan.FromMilliseconds(100));
            var completed = await Task.WhenAny(disposeTask, observationWindow);
            await Assert.That(ReferenceEquals(completed, observationWindow)).IsTrue();
            await Assert.That(() => lifetime.Acquire(this)).Throws<ObjectDisposedException>();
        }
        finally
        {
            heldResource.Dispose();
            var completed = await Task.WhenAny(disposeTask, Task.Delay(TimeSpan.FromSeconds(5)));
            await Assert.That(ReferenceEquals(completed, disposeTask)).IsTrue();
            await disposeTask;
        }
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
    public async Task SessionOperationGate_QueuedAdmissionIsAsyncAndCancelable()
    {
        var gate = new SessionOperationGate();
        using var active = gate.Acquire(this, assignCurrentThread: false);
        using var cts = new CancellationTokenSource();
        var queued = gate.AcquireAsync(this, cts.Token);
        await Assert.That(queued.IsCompleted).IsFalse();
        cts.Cancel();
        await Assert.ThrowsAsync<OperationCanceledException>(() => queued);

        active.Dispose();
        using var reuse = await gate.AcquireAsync(this, CancellationToken.None);
    }

    [Test]
    public async Task SessionOperationGate_PreCanceledAdmissionDoesNotWaitOrTakeOwnership()
    {
        var gate = new SessionOperationGate();
        using var active = gate.Acquire(this, assignCurrentThread: false);
        using var cts = new CancellationTokenSource();
        cts.Cancel();
        await Assert.ThrowsAsync<OperationCanceledException>(() => gate.AcquireAsync(this, cts.Token));

        active.Dispose();
        using var reuse = await gate.AcquireAsync(this, CancellationToken.None);
    }

    [Test]
    public async Task SessionOperationGate_CloseWakesQueuedAsyncAdmission()
    {
        var gate = new SessionOperationGate();
        using var active = gate.Acquire(this, assignCurrentThread: false);
        var queued = gate.AcquireAsync(this, CancellationToken.None);
        await Assert.That(queued.IsCompleted).IsFalse();
        gate.BeginClose();
        await Assert.ThrowsAsync<ObjectDisposedException>(() => queued);
        active.Dispose();
        gate.WaitForOperations();
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
