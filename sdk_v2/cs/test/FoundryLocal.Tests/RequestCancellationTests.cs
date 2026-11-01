// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.AI.Foundry.Local.Detail.Native;

using NativeModel = Detail.Native.Model;
using NativeSession = Detail.Native.Session;

internal sealed class RequestCancellationTests
{
    [Test]
    [NotInParallel]
    public async Task TokenCancellation_AfterNativeSuccess_PreservesResponse()
    {
        Api.EnsureInitialized();
        var originalInference = Api.Inference;
        using var cts = new CancellationTokenSource();
        var responsePtr = new IntPtr(3);
        var responseReleases = 0;
        try
        {
            Api.Inference.SessionCreate = (model, out session) =>
            {
                session = new IntPtr(1);
                return IntPtr.Zero;
            };
            Api.Inference.SessionRelease = _ => { };
            Api.Inference.RequestCancel = _ => IntPtr.Zero;
            Api.Inference.ResponseRelease = _ => responseReleases++;
            Api.Inference.SessionProcessRequest = (session, request, ref response) =>
            {
                response = responsePtr;
                // Simulate cancellation after native commit, before the successful call returns.
                cts.Cancel();
                return IntPtr.Zero;
            };

            using var session = new NativeSession(new NativeModel(new IntPtr(2)));
            var response = session.ProcessRequest(new IntPtr(4), cts.Token);

            await Assert.That(cts.IsCancellationRequested).IsTrue();
            await Assert.That(response).IsEqualTo(responsePtr);
            await Assert.That(responseReleases).IsEqualTo(0);
        }
        finally
        {
            Api.Inference = originalInference;
        }
    }

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
