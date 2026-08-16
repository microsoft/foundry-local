// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Reflection;
using System.Runtime.Serialization;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;

/// <summary>
/// Model-free tests for stream-vs-processing isolation.
/// </summary>
internal sealed class SessionConcurrencyTests
{
    private const BindingFlags PrivateInstance = BindingFlags.Instance | BindingFlags.NonPublic;

    [Test]
    public async Task ActiveStream_BlocksBothRequestKinds()
    {
        var session = CreateSession();
        SetSessionField(session, "_streamingActive", true);
        SetSessionField(session, "_activeCalls", 1);
        var request = CreateRequest(disposed: true);

        await Assert.That(() => session.ProcessStreamingRequestAsync(request))
            .Throws<InvalidOperationException>();
        await Assert.That(async () => await session.ProcessRequestAsync(request).ConfigureAwait(false))
            .Throws<InvalidOperationException>();

        await AssertProcessingState(session, activeCalls: 1, nonStreamingCalls: 0, streamingActive: true);
    }

    [Test]
    public async Task ActiveNonStreaming_BlocksStreamButAllowsAnotherNonStreamingClaim()
    {
        var session = CreateSession();
        SetSessionField(session, "_activeNonStreamingCalls", 1);
        SetSessionField(session, "_activeCalls", 1);
        var request = CreateRequest(disposed: true);

        await Assert.That(() => session.ProcessStreamingRequestAsync(request))
            .Throws<InvalidOperationException>();
        await Assert.That(async () => await session.ProcessRequestAsync(request).ConfigureAwait(false))
            .Throws<ObjectDisposedException>();

        // The accepted non-streaming claim is released when acquiring the Request lease fails.
        await AssertProcessingState(session, activeCalls: 1, nonStreamingCalls: 1, streamingActive: false);
    }

    [Test]
    public async Task FailedStreamingSetup_ReleasesSessionAndRequestClaims()
    {
        var session = CreateSession();
        var disposedRequest = CreateRequest(disposed: true);

        await Assert.That(() => session.ProcessStreamingRequestAsync(disposedRequest))
            .Throws<ObjectDisposedException>();
        await AssertProcessingState(session, activeCalls: 0, nonStreamingCalls: 0, streamingActive: false);

        var leasedRequest = CreateRequest(disposed: false);
        SetSessionField(session, "_activeChannel", Channel.CreateUnbounded<Item>());

        await Assert.That(() => session.ProcessStreamingRequestAsync(leasedRequest))
            .Throws<InvalidOperationException>();

        await AssertProcessingState(session, activeCalls: 0, nonStreamingCalls: 0, streamingActive: false);
        await Assert.That(GetRequestField<int>(leasedRequest, "_activeProcesses")).IsEqualTo(0);
    }

    [Test]
    public async Task ClearStreamingState_DoesNotClearNewerRequest()
    {
        var session = CreateSession();
        using var older = new CancellationTokenSource();
        using var newer = new CancellationTokenSource();
        SetSessionField(session, "_activeStreamingCts", newer);

        session.ClearStreamingState(older);
        await Assert.That(GetSessionField<CancellationTokenSource?>(session, "_activeStreamingCts"))
            .IsSameReferenceAs(newer);

        session.ClearStreamingState(newer);
        await Assert.That(GetSessionField<CancellationTokenSource?>(session, "_activeStreamingCts")).IsNull();
    }

    private static ChatSession CreateSession()
    {
#pragma warning disable SYSLIB0050 // Required by the net462 test target to avoid invoking the native constructor.
        var session = (ChatSession)FormatterServices.GetUninitializedObject(typeof(ChatSession));
#pragma warning restore SYSLIB0050
        SetSessionField(session, "_gate", new object());
        SetSessionField(
            session,
            "_nativeStreamingCallback",
            (Detail.Interop.FlStreamingCallback)((_, _) => 0));
        return session;
    }

    private static Request CreateRequest(bool disposed)
    {
#pragma warning disable SYSLIB0050 // Required by the net462 test target to avoid invoking the native constructor.
        var request = (Request)FormatterServices.GetUninitializedObject(typeof(Request));
#pragma warning restore SYSLIB0050
        SetRequestField(request, "_lifetimeGate", new object());
        SetRequestField(request, "_disposeRequested", disposed);
        return request;
    }

    private static async Task AssertProcessingState(
        Session session,
        int activeCalls,
        int nonStreamingCalls,
        bool streamingActive)
    {
        await Assert.That(GetSessionField<int>(session, "_activeCalls")).IsEqualTo(activeCalls);
        await Assert.That(GetSessionField<int>(session, "_activeNonStreamingCalls")).IsEqualTo(nonStreamingCalls);
        await Assert.That(GetSessionField<bool>(session, "_streamingActive")).IsEqualTo(streamingActive);
    }

    private static T GetSessionField<T>(Session session, string name) =>
        (T)typeof(Session).GetField(name, PrivateInstance)!.GetValue(session)!;

    private static void SetSessionField(Session session, string name, object? value) =>
        typeof(Session).GetField(name, PrivateInstance)!.SetValue(session, value);

    private static T GetRequestField<T>(Request request, string name) =>
        (T)typeof(Request).GetField(name, PrivateInstance)!.GetValue(request)!;

    private static void SetRequestField(Request request, string name, object? value) =>
        typeof(Request).GetField(name, PrivateInstance)!.SetValue(request, value);
}
