// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;
using System;
using System.Diagnostics;

using Microsoft.Extensions.Logging;

public class FoundryLocalException : Exception
{
    public FoundryLocalException(string message) : base(message)
    {
    }

    public FoundryLocalException(string message, FoundryLocalErrorCode errorCode) : base(message)
    {
        ErrorCode = errorCode;
    }

    public FoundryLocalException(string message, Exception innerException) : base(message, innerException)
    {
    }

    internal FoundryLocalException(string message, ILogger logger) : base(message)
    {
        Debug.Assert(logger != null);
        logger!.LogError(message);
    }

    internal FoundryLocalException(string message, Exception innerException, ILogger logger)
        : base(message, innerException)
    {
        Debug.Assert(logger != null);
        logger!.LogError(innerException, message);
    }

    /// <summary>
    /// Gets the native error code associated with this failure, or <c>null</c> for SDK-side failures that
    /// have no corresponding native error code.
    /// </summary>
    public FoundryLocalErrorCode? ErrorCode { get; }
}
