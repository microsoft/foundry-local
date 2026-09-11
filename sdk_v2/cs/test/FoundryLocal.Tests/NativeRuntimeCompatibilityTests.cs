// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.AI.Foundry.Local.Detail.Native;

internal sealed class NativeRuntimeCompatibilityTests
{
    [Test]
    public async Task IncompatibleRuntimeMessageIncludesLoadedAndRequiredVersions()
    {
        const string loadedRuntimeVersion = "0.5.0-test";

        var message = Api.CreateIncompatibleRuntimeMessage(loadedRuntimeVersion);

        await Assert.That(message).IsEqualTo(
            $"FoundryLocalGetApi({NativeMethods.ApiVersion}) returned null. The loaded native Foundry Local runtime "
            + "reports product version '0.5.0-test', but this SDK requires C API table version "
            + $"{NativeMethods.ApiVersion}. Update the native runtime ({NativeMethods.LibraryName} and the libraries "
            + "shipped with it).");
    }
}
