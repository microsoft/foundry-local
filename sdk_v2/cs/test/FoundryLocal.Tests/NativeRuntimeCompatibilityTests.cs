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
            "FoundryLocalGetApi returned null: loaded native Foundry Local runtime version "
            + "'0.5.0-test' is incompatible with this build of the SDK. Required C API version: "
            + $"{NativeMethods.ApiVersion}. Update the native Foundry Local runtime ({NativeMethods.LibraryName} "
            + "and the libraries shipped with it).");
    }
}
