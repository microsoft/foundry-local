// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Threading.Tasks;

/// <summary>
/// Tests for <see cref="FoundryLocalException"/> error-code surfacing. These require neither the native
/// library nor a downloaded model.
/// </summary>
internal sealed class FoundryLocalExceptionTests
{
    [Test]
    public async Task Constructor_WithErrorCode_SetsErrorCodeAndMessage()
    {
        var ex = new FoundryLocalException("boom", FoundryLocalErrorCode.InvalidUsage);

        await Assert.That(ex.ErrorCode).IsEqualTo(FoundryLocalErrorCode.InvalidUsage);
        await Assert.That(ex.Message).IsEqualTo("boom");
    }

    [Test]
    public async Task Constructor_MessageOnly_LeavesErrorCodeNull()
    {
        var ex = new FoundryLocalException("x");

        await Assert.That(ex.ErrorCode).IsNull();
        await Assert.That(ex.Message).IsEqualTo("x");
    }
}
