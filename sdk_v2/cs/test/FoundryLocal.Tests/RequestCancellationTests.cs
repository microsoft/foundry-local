// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using Microsoft.AI.Foundry.Local.Detail;

internal sealed class RequestCancellationTests
{
    [Test]
    public async Task TokenCancellation_RetriesAcrossNativeAdmission()
    {
        using var cts = new CancellationTokenSource();
        var admitted = 0;
        var attempts = 0;
        var canceled = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        using var cancellation = new RequestCancellation(() =>
        {
            Interlocked.Increment(ref attempts);
            if (Volatile.Read(ref admitted) != 0)
            {
                canceled.TrySetResult(true);
            }
        }, cts.Token);

        cts.Cancel();
        await Assert.That(Volatile.Read(ref attempts)).IsGreaterThan(0);
        await Assert.That(canceled.Task.IsCompleted).IsFalse();

        Volatile.Write(ref admitted, 1);
        var completed = await Task.WhenAny(canceled.Task, Task.Delay(TimeSpan.FromSeconds(2))).ConfigureAwait(false);
        await Assert.That(completed == canceled.Task).IsTrue();
        await Assert.That(Volatile.Read(ref attempts)).IsGreaterThan(1);
    }

    [Test]
    public async Task Disposal_JoinsCancellationBeforeRequestRelease()
    {
        using var cts = new CancellationTokenSource();
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var attempts = 0;
        using var cancellation = new RequestCancellation(() =>
        {
            Interlocked.Increment(ref attempts);
            entered.Set();
            release.Wait(TimeSpan.FromSeconds(5));
        }, cts.Token);

        var cancelTask = Task.Run(cts.Cancel);
        try
        {
            await Assert.That(entered.Wait(TimeSpan.FromSeconds(2))).IsTrue();
            var disposeTask = Task.Run(cancellation.Dispose);
            await Task.Delay(25).ConfigureAwait(false);
            await Assert.That(disposeTask.IsCompleted).IsFalse();
            release.Set();
            await Task.WhenAll(cancelTask, disposeTask).ConfigureAwait(false);
            var attemptsAtRelease = Volatile.Read(ref attempts);
            await Task.Delay(25).ConfigureAwait(false);
            await Assert.That(Volatile.Read(ref attempts)).IsEqualTo(attemptsAtRelease);
        }
        finally
        {
            release.Set();
        }
    }
}
