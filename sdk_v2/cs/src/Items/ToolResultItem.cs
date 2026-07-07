// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;
using System.Runtime.InteropServices;

using Microsoft.AI.Foundry.Local.Detail.Interop;
using Microsoft.AI.Foundry.Local.Detail.Native;

public sealed class ToolResultItem : Item
{
    /// <summary>Identifier of the tool call this result satisfies. Always present.</summary>
    public string CallId { get; }

    /// <summary>Tool output text. Empty string when the tool produced no content.</summary>
    public string Result { get; }

    public ToolResultItem(string callId, string result) : base(ItemType.ToolResult)
    {
        CallId = callId;
        Result = result;

        var callIdNative = Detail.Utf8.StringToCoTaskMem(callId);
        var resultNative = Detail.Utf8.StringToCoTaskMem(result);

        try
        {
            var data = new FlToolResultData
            {
                Version = NativeMethods.ApiVersion,
                CallId = callIdNative,
                Result = resultNative,
            };
            Api.CheckStatus(Api.Item.SetToolResult(Ptr, ref data));
        }
        finally
        {
            Marshal.FreeCoTaskMem(callIdNative);
            Marshal.FreeCoTaskMem(resultNative);
        }
    }

    internal ToolResultItem(IntPtr ptr, bool ownsHandle) : base(ptr, ownsHandle)
    {
        var status = Api.Item.GetToolResult(Ptr, out var toolResult);
        Api.CheckStatus(status);

        // Native side stores std::string and returns c_str() — pointers are never null.
        CallId = Detail.Utf8.PtrToString(toolResult.CallId)!;
        Result = Detail.Utf8.PtrToString(toolResult.Result)!;
    }
}
