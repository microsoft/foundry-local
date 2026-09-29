// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Runtime.InteropServices;
using System.Threading.Tasks;

using Microsoft.AI.Foundry.Local.Detail.Interop;

#pragma warning disable TUnitAssertions0005

internal sealed class RequestPreflightAbiTests
{
    [Test]
    public async Task ResultLayout_MatchesNativeNaturalAlignment()
    {
        await Assert.That(NativeMethods.ApiVersion).IsEqualTo(2u);
        await Assert.That(Marshal.OffsetOf<FlRequestPreflightResult>(
            nameof(FlRequestPreflightResult.Version)).ToInt64()).IsEqualTo(0L);
        await Assert.That(Marshal.OffsetOf<FlRequestPreflightResult>(
            nameof(FlRequestPreflightResult.PromptTokens)).ToInt64()).IsEqualTo(8L);
        await Assert.That(Marshal.OffsetOf<FlRequestPreflightResult>(
            nameof(FlRequestPreflightResult.OutputReserveTokens)).ToInt64()).IsEqualTo(16L);
        await Assert.That(Marshal.OffsetOf<FlRequestPreflightResult>(
            nameof(FlRequestPreflightResult.RequiredTokens)).ToInt64()).IsEqualTo(24L);
        await Assert.That(Marshal.OffsetOf<FlRequestPreflightResult>(
            nameof(FlRequestPreflightResult.ContextLimitTokens)).ToInt64()).IsEqualTo(32L);
        await Assert.That(Marshal.OffsetOf<FlRequestPreflightResult>(
            nameof(FlRequestPreflightResult.Fits)).ToInt64()).IsEqualTo(40L);
        await Assert.That(Marshal.OffsetOf<FlRequestPreflightResult>(
            nameof(FlRequestPreflightResult.DeficitTokens)).ToInt64()).IsEqualTo(48L);
        await Assert.That(Marshal.SizeOf<FlRequestPreflightResult>()).IsEqualTo(56);
    }

    [Test]
    public async Task InferenceVtable_PreflightSlotsAreAppendedInHeaderOrder()
    {
        var pointerSize = IntPtr.Size;

        await Assert.That(Marshal.OffsetOf<FlInferenceApi>(
            nameof(FlInferenceApi.SessionCreateRequestPreflight)).ToInt64())
            .IsEqualTo(22L * pointerSize);
        await Assert.That(Marshal.OffsetOf<FlInferenceApi>(
            nameof(FlInferenceApi.RequestPreflightExecute)).ToInt64())
            .IsEqualTo(23L * pointerSize);
        await Assert.That(Marshal.OffsetOf<FlInferenceApi>(
            nameof(FlInferenceApi.RequestPreflightRelease)).ToInt64())
            .IsEqualTo(24L * pointerSize);
        await Assert.That(Marshal.SizeOf<FlInferenceApi>()).IsEqualTo(25 * pointerSize);
    }

}

#pragma warning restore TUnitAssertions0005
