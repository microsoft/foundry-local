// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

internal sealed class ToolItemValidationTests
{
    [Test]
    [Arguments("callId")]
    [Arguments("name")]
    [Arguments("arguments")]
    public async Task ToolCallItem_RejectsEmbeddedNulBeforeNativeCall(string argument)
    {
        var exception = await Assert.That(() => new ToolCallItem(
            argument == "callId" ? "bad\0id" : "call",
            argument == "name" ? "bad\0name" : "tool",
            argument == "arguments" ? "bad\0arguments" : "{}")).Throws<ArgumentException>();

        await Assert.That(exception!.ParamName).IsEqualTo(argument);
    }

    [Test]
    [Arguments("callId")]
    [Arguments("result")]
    public async Task ToolResultItem_RejectsEmbeddedNulBeforeNativeCall(string argument)
    {
        var exception = await Assert.That(() => new ToolResultItem(
            argument == "callId" ? "bad\0id" : "call",
            argument == "result" ? "bad\0result" : string.Empty)).Throws<ArgumentException>();

        await Assert.That(exception!.ParamName).IsEqualTo(argument);
    }

    [Test]
    public async Task ToolPayloads_PreserveEmptyStrings()
    {
        using var call = new ToolCallItem(string.Empty, string.Empty, string.Empty);
        using var result = new ToolResultItem(string.Empty, string.Empty);

        await Assert.That(call.CallId).IsEmpty();
        await Assert.That(call.Name).IsEmpty();
        await Assert.That(call.Arguments).IsEmpty();
        await Assert.That(result.CallId).IsEmpty();
        await Assert.That(result.Result).IsEmpty();
    }
}
