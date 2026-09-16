// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Runtime.Serialization;

internal sealed class ToolDefinitionValidationTests
{
    [Test]
    [Arguments("name")]
    [Arguments("description")]
    [Arguments("jsonSchema")]
    public async Task FunctionToolDefinition_RejectsEmbeddedNulBeforeNativeCall(string argument)
    {
        var session = CreateUninitializedSession();

        var exception = await Assert.That(() => session.AddToolDefinition(
            argument == "name" ? "bad\0name" : "name",
            argument == "description" ? "bad\0description" : "description",
            argument == "jsonSchema" ? "{\0}" : "{}")).Throws<ArgumentException>();

        await Assert.That(exception!.ParamName).IsEqualTo(argument);
    }

    [Test]
    [Arguments("name")]
    [Arguments("description")]
    public async Task CustomToolDefinition_RejectsEmbeddedNulBeforeNativeCall(string argument)
    {
        var session = CreateUninitializedSession();

        var exception = await Assert.That(() => session.AddCustomToolDefinition(
            argument == "name" ? "bad\0name" : "name",
            argument == "description" ? "bad\0description" : "description")).Throws<ArgumentException>();

        await Assert.That(exception!.ParamName).IsEqualTo(argument);
    }

    [Test]
    public async Task RemoveToolDefinition_RejectsEmbeddedNulBeforeNativeCall()
    {
        var session = CreateUninitializedSession();

        var exception =
            await Assert.That(() => session.RemoveToolDefinition("bad\0name")).Throws<ArgumentException>();

        await Assert.That(exception!.ParamName).IsEqualTo("toolName");
    }

    private static ChatSession CreateUninitializedSession()
    {
#pragma warning disable SYSLIB0050 // Model-free validation test; no native session is accessed.
        return (ChatSession)FormatterServices.GetUninitializedObject(typeof(ChatSession));
#pragma warning restore SYSLIB0050
    }
}
