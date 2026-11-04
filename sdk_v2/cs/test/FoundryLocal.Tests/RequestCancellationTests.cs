// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.AI.Foundry.Local.Detail.Native;
using Microsoft.Extensions.Logging.Abstractions;

using Model = Microsoft.AI.Foundry.Local.Model;
using NativeModel = Detail.Native.Model;
using NativeSession = Detail.Native.Session;

internal sealed class RequestCancellationTests
{
    [Test]
    [NotInParallel]
    [Arguments(false)]
    [Arguments(true)]
    public async Task TokenCancellation_AfterNativeSuccess_PreservesResponse(bool streaming)
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
            Api.Inference.SessionSetStreamingCallback = (_, _, _) => IntPtr.Zero;
            Api.Inference.RequestCreate = (out request) =>
            {
                request = new IntPtr(4);
                return IntPtr.Zero;
            };
            Api.Inference.RequestRelease = _ => { };
            Api.Inference.RequestCancel = _ => IntPtr.Zero;
            Api.Inference.ResponseRelease = _ => responseReleases++;
            Api.Inference.SessionProcessRequest = (session, request, ref response) =>
            {
                response = responsePtr;
                // Simulate cancellation after native commit, before the successful call returns.
                cts.Cancel();
                return IntPtr.Zero;
            };

            using var request = new Request();
            IntPtr response;
            if (streaming)
            {
                using var lifetime = new ManagerLifetime();
                var model = new Model(new NativeModel(new IntPtr(2)), NullLogger.Instance, lifetime);
                using var session = new TestSession(model);
                session.SetStreaming(true);
                await using var stream = session.ProcessStreamingRequestAsync(request, cts.Token);
                using var final = await stream.FinalResponse;
                response = final.Ptr;
                await Assert.That(responseReleases).IsEqualTo(0);
            }
            else
            {
                using var session = new NativeSession(new NativeModel(new IntPtr(2)));
                response = session.ProcessRequest(request, cts.Token);
            }

            await Assert.That(cts.IsCancellationRequested).IsTrue();
            await Assert.That(response).IsEqualTo(responsePtr);
            await Assert.That(responseReleases).IsEqualTo(streaming ? 1 : 0);
        }
        finally
        {
            Api.Inference = originalInference;
        }
    }

    [Test]
    [NotInParallel]
    public async Task Cancellation_WithOnePoolWorker_RetriesAndDisposesWithoutDeadlock()
    {
        var completed = await Task.Factory.StartNew(() =>
        {
            ThreadPool.GetMinThreads(out var minWorkers, out var minIo);
            ThreadPool.GetMaxThreads(out var maxWorkers, out var maxIo);
            using var cts = new CancellationTokenSource();
            using var canceled = new ManualResetEventSlim();
            using var entered = new ManualResetEventSlim();
            using var start = new ManualResetEventSlim();
            var admitted = 0;
            Task? inference = null;
            bool completedBeforeRestore;
            try
            {
                inference = Task.Run(() =>
                {
                    entered.Set();
                    start.Wait(TimeSpan.FromSeconds(5));
                    using var cancellation = new RequestCancellation(() =>
                    {
                        if (Volatile.Read(ref admitted) != 0)
                        {
                            canceled.Set();
                        }
                    }, cts.Token);

                    cts.Cancel();
                    Volatile.Write(ref admitted, 1);
                    canceled.Wait(TimeSpan.FromSeconds(2));
                });

                if (!entered.Wait(TimeSpan.FromSeconds(5)))
                {
                    throw new InvalidOperationException("Inference worker did not start.");
                }

                if (!ThreadPool.SetMinThreads(1, minIo) || !ThreadPool.SetMaxThreads(1, maxIo))
                {
                    throw new InvalidOperationException("Cannot cap the thread pool for this regression.");
                }

                start.Set();
                completedBeforeRestore = inference.Wait(TimeSpan.FromSeconds(3));
            }
            finally
            {
                ThreadPool.SetMaxThreads(maxWorkers, maxIo);
                ThreadPool.SetMinThreads(minWorkers, minIo);
                start.Set();
                inference?.GetAwaiter().GetResult();
            }

            return completedBeforeRestore && canceled.IsSet;
        }, CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default).ConfigureAwait(false);

        await Assert.That(completed).IsTrue();
    }

    [Test]
    [NotInParallel]
    public async Task CompetingRequestToken_DoesNotCancelExistingCall()
    {
        Api.EnsureInitialized();
        var originalInference = Api.Inference;
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        using var cts = new CancellationTokenSource();
        var cancelCalls = 0;
        var processCalls = 0;
        Task<IntPtr>? active = null;
        try
        {
            Api.Inference.SessionCreate = (model, out session) =>
            {
                session = new IntPtr(1);
                return IntPtr.Zero;
            };
            Api.Inference.SessionRelease = _ => { };
            Api.Inference.RequestCreate = (out request) =>
            {
                request = new IntPtr(4);
                return IntPtr.Zero;
            };
            Api.Inference.RequestRelease = _ => { };
            Api.Inference.RequestCancel = _ =>
            {
                Interlocked.Increment(ref cancelCalls);
                return IntPtr.Zero;
            };
            Api.Inference.SessionProcessRequest = (session, request, ref response) =>
            {
                Interlocked.Increment(ref processCalls);
                entered.Set();
                release.Wait(TimeSpan.FromSeconds(5));
                response = new IntPtr(3);
                return IntPtr.Zero;
            };

            using var first = new NativeSession(new NativeModel(new IntPtr(2)));
            using var second = new NativeSession(new NativeModel(new IntPtr(2)));
            using var request = new Request();
            active = Task.Run(() => first.ProcessRequest(request));
            try
            {
                await Assert.That(entered.Wait(TimeSpan.FromSeconds(2))).IsTrue();
                await Assert.ThrowsAsync<InvalidOperationException>(
                    () => Task.FromResult(second.ProcessRequest(request, cts.Token)));
                cts.Cancel();
                await Assert.That(Volatile.Read(ref cancelCalls)).IsEqualTo(0);
                await Assert.That(Volatile.Read(ref processCalls)).IsEqualTo(1);
            }
            finally
            {
                release.Set();
                await active.ConfigureAwait(false);
            }

            await Assert.That(second.ProcessRequest(request)).IsEqualTo(new IntPtr(3));
        }
        finally
        {
            Api.Inference = originalInference;
        }
    }

    [Test]
    [NotInParallel]
    public async Task ProcessingOwnership_IsHeldThroughCancellationCleanup()
    {
        Api.EnsureInitialized();
        var originalInference = Api.Inference;
        using var nativeReturned = new ManualResetEventSlim();
        using var cancelEntered = new ManualResetEventSlim();
        using var releaseCancel = new ManualResetEventSlim();
        using var cts = new CancellationTokenSource();
        Task? cancelTask = null;
        try
        {
            Api.Inference.SessionCreate = (model, out session) =>
            {
                session = new IntPtr(1);
                return IntPtr.Zero;
            };
            Api.Inference.SessionRelease = _ => { };
            Api.Inference.RequestCreate = (out request) =>
            {
                request = new IntPtr(4);
                return IntPtr.Zero;
            };
            Api.Inference.RequestRelease = _ => { };
            Api.Inference.RequestCancel = _ =>
            {
                cancelEntered.Set();
                releaseCancel.Wait(TimeSpan.FromSeconds(5));
                return IntPtr.Zero;
            };
            Api.Inference.SessionProcessRequest = (session, request, ref response) =>
            {
                cancelTask = Task.Run(cts.Cancel);
                cancelEntered.Wait(TimeSpan.FromSeconds(2));
                response = new IntPtr(3);
                nativeReturned.Set();
                return IntPtr.Zero;
            };

            using var session = new NativeSession(new NativeModel(new IntPtr(2)));
            using var request = new Request();
            var active = Task.Run(() => session.ProcessRequest(request, cts.Token));
            try
            {
                await Assert.That(nativeReturned.Wait(TimeSpan.FromSeconds(2))).IsTrue();
                await Assert.That(active.IsCompleted).IsFalse();
                await Assert.ThrowsAsync<InvalidOperationException>(
                    () => Task.FromResult(session.ProcessRequest(request)));
            }
            finally
            {
                releaseCancel.Set();
                await active.ConfigureAwait(false);
                if (cancelTask != null)
                {
                    await cancelTask.ConfigureAwait(false);
                }
            }

            using var reuse = request.AcquireProcessingLease();
            await Assert.That(reuse.Ptr).IsEqualTo(new IntPtr(4));
        }
        finally
        {
            Api.Inference = originalInference;
        }
    }

    [Test]
    [NotInParallel]
    public async Task QueuedTokenCancellation_ReleasesRequestWithoutCancelingOwner()
    {
        Api.EnsureInitialized();
        var originalInference = Api.Inference;
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        using var submitted = new ManualResetEventSlim();
        using var cts = new CancellationTokenSource();
        var cancelCalls = 0;
        var processCalls = 0;
        try
        {
            Api.Inference.SessionCreate = (model, out session) =>
            {
                session = new IntPtr(1);
                return IntPtr.Zero;
            };
            Api.Inference.SessionRelease = _ => { };
            Api.Inference.RequestCreate = (out request) =>
            {
                request = new IntPtr(4);
                return IntPtr.Zero;
            };
            Api.Inference.RequestRelease = _ => { };
            Api.Inference.RequestCancel = _ =>
            {
                Interlocked.Increment(ref cancelCalls);
                return IntPtr.Zero;
            };
            Api.Inference.ResponseRelease = _ => { };
            Api.Inference.SessionProcessRequest = (session, request, ref response) =>
            {
                if (Interlocked.Increment(ref processCalls) == 1)
                {
                    entered.Set();
                    release.Wait(TimeSpan.FromSeconds(5));
                }

                response = new IntPtr(3);
                return IntPtr.Zero;
            };

            using var lifetime = new ManagerLifetime();
            var model = new Model(new NativeModel(new IntPtr(2)), NullLogger.Instance, lifetime);
            using var session = new TestSession(model);
            using var first = new Request();
            using var second = new Request();
            var active = session.ProcessRequestAsync(first);
            Task<Response>? queued = null;
            try
            {
                await Assert.That(entered.Wait(TimeSpan.FromSeconds(2))).IsTrue();
                queued = Task.Run(async () =>
                {
                    var pending = session.ProcessRequestAsync(second, cts.Token);
                    submitted.Set();
                    return await pending.ConfigureAwait(false);
                });
                await Assert.That(submitted.Wait(TimeSpan.FromSeconds(2))).IsTrue();
                cts.Cancel();
                var completed = await Task.WhenAny(queued, Task.Delay(TimeSpan.FromSeconds(2))).ConfigureAwait(false);
                await Assert.That(completed == queued).IsTrue();
                await Assert.ThrowsAsync<OperationCanceledException>(() => queued);
                await Assert.That(active.IsCompleted).IsFalse();
                await Assert.That(cancelCalls).IsEqualTo(0);
                using var reuse = second.AcquireProcessingLease();
            }
            finally
            {
                release.Set();
                using var response = await active.ConfigureAwait(false);
                if (queued != null)
                {
                    try { using var unexpected = await queued.ConfigureAwait(false); }
                    catch (OperationCanceledException) { }
                }
            }

            using var recovered = await session.ProcessRequestAsync(second).ConfigureAwait(false);
            await Assert.That(processCalls).IsEqualTo(2);
        }
        finally
        {
            Api.Inference = originalInference;
        }
    }

    [Test]
    public async Task ConcurrentCancellation_UsesOneRetryThreadAndStopsBeforeRelease()
    {
        using var cts = new CancellationTokenSource();
        using var retried = new CountdownEvent(16);
        var cancellationThread = Environment.CurrentManagedThreadId;
        var retryThreads = new System.Collections.Concurrent.ConcurrentDictionary<int, bool>();
        var attempts = new int[16];
        var observed = new int[16];
        var helpers = Enumerable.Range(0, attempts.Length).Select(index =>
            new RequestCancellation(() =>
            {
                Interlocked.Increment(ref attempts[index]);
                var thread = Environment.CurrentManagedThreadId;
                if (thread != cancellationThread)
                {
                    retryThreads.TryAdd(thread, true);
                    if (Interlocked.Exchange(ref observed[index], 1) == 0)
                    {
                        retried.Signal();
                    }
                }
            }, cts.Token)).ToArray();
        try
        {
            cts.Cancel();
            await Assert.That(retried.Wait(TimeSpan.FromSeconds(2))).IsTrue();
            await Assert.That(retryThreads.Count).IsEqualTo(1);
        }
        finally
        {
            foreach (var helper in helpers)
            {
                helper.Dispose();
            }
        }

        var total = attempts.Sum();
        await Task.Delay(25).ConfigureAwait(false);
        await Assert.That(attempts.Sum()).IsEqualTo(total);
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

    private sealed class TestSession : Microsoft.AI.Foundry.Local.Session
    {
        internal TestSession(IModel model) : base(model) { }
    }
}
