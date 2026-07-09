// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;
using System.Runtime.InteropServices;

using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.AI.Foundry.Local.Detail.Native;

public sealed class ToolCallItem : Item
{
    /// <summary>Identifier the model assigned to this tool call. Always present.</summary>
    public string CallId { get; }

    /// <summary>Name of the tool the model is requesting. Always present.</summary>
    public string Name { get; }

    /// <summary>JSON-encoded arguments for the call. Empty string when the tool takes no parameters.</summary>
    public string Arguments { get; }

    public ToolCallItem(string callId, string name, string arguments)
        : base(ItemType.ToolCall)
    {
        CallId = callId;
        Name = name;
        Arguments = arguments;

        var callIdNative = Detail.Utf8.StringToCoTaskMem(callId);
        var nameNative = Detail.Utf8.StringToCoTaskMem(name);
        var argsNative = Detail.Utf8.StringToCoTaskMem(arguments);

        try
        {
            var data = new FlToolCallData
            {
                Version = NativeMethods.ApiVersion,
                CallId = callIdNative,
                Name = nameNative,
                Arguments = argsNative,
            };
            Api.CheckStatus(Api.Item.SetToolCall(Ptr, ref data));
        }
        finally
        {
            Marshal.FreeCoTaskMem(callIdNative);
            Marshal.FreeCoTaskMem(nameNative);
            Marshal.FreeCoTaskMem(argsNative);
        }
    }

    internal ToolCallItem(IntPtr ptr, bool ownsHandle) : base(ptr, ownsHandle)
    {
        var status = Api.Item.GetToolCall(Ptr, out var toolCall);
        Api.CheckStatus(status);

        // Native side stores std::string and returns c_str() — pointers are never null.
        CallId = Detail.Utf8.PtrToString(toolCall.CallId)!;
        Name = Detail.Utf8.PtrToString(toolCall.Name)!;
        Arguments = Detail.Utf8.PtrToString(toolCall.Arguments)!;
    }
}
