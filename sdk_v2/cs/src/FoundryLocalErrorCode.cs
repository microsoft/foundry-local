// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

/// <summary>
/// Stable error codes reported by the native Foundry Local runtime. These mirror the native ABI error codes
/// 1:1 (same names and numeric values) and are surfaced on <see cref="FoundryLocalException.ErrorCode"/> so
/// callers can inspect the underlying cause of a native failure without depending on internal interop types.
/// </summary>
public enum FoundryLocalErrorCode
{
    /// <summary>No error.</summary>
    Ok = 0,

    /// <summary>The requested operation is not implemented.</summary>
    NotImplemented = 1,

    /// <summary>An internal error occurred in the native runtime.</summary>
    Internal = 2,

    /// <summary>An argument passed to the native runtime was invalid.</summary>
    InvalidArgument = 3,

    /// <summary>The API was used incorrectly (e.g. invalid call sequence or state).</summary>
    InvalidUsage = 4,

    /// <summary>The operation was cancelled.</summary>
    OperationCancelled = 5,

    /// <summary>A network error occurred.</summary>
    Network = 6,
}
