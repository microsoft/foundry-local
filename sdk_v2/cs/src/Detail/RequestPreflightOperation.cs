// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Detail.Native;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.AI.Foundry.Local.Detail.Interop;

/// <summary>
/// Owns a one-shot native request-preflight operation.
/// </summary>
internal sealed class RequestPreflightOperation : IDisposable
{
    private IntPtr _ptr;
    private readonly ManagerLifetime.Lease _managerLease;

    internal RequestPreflightOperation(IntPtr ptr, ManagerLifetime.Lease managerLease)
    {
        _ptr = ptr;
        _managerLease = managerLease;
    }

    internal RequestPreflightResult Execute()
    {
        var nativeResult = new FlRequestPreflightResult
        {
            Version = NativeMethods.ApiVersion,
        };

        Api.CheckStatus(Api.Inference.RequestPreflightExecute(_ptr, ref nativeResult));

        return new RequestPreflightResult(
            nativeResult.PromptTokens,
            nativeResult.OutputReserveTokens,
            nativeResult.RequiredTokens,
            nativeResult.ContextLimitTokens,
            nativeResult.Fits,
            nativeResult.DeficitTokens);
    }

    public void Dispose()
    {
        var ptr = Interlocked.Exchange(ref _ptr, IntPtr.Zero);
        try
        {
            if (ptr != IntPtr.Zero)
            {
                Api.Inference.RequestPreflightRelease(ptr);
            }
        }
        finally
        {
#pragma warning disable IDISP007 // The operation owns the manager lease transferred at capture.
            _managerLease.Dispose();
#pragma warning restore IDISP007
        }
    }
}
