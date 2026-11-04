// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

/// <summary>A coherent, read-only snapshot of HTTP work in the SDK web service.</summary>
public readonly struct WebServiceActivity
{
    internal WebServiceActivity(ulong activeRequests, TimeSpan idleDuration)
    {
        ActiveRequests = activeRequests;
        IdleDuration = idleDuration;
    }

    /// <summary>HTTP requests still delivering a response or completing associated streaming work.</summary>
    public ulong ActiveRequests { get; }

    /// <summary>
    /// Monotonic elapsed time since the last HTTP completion, or service start before the first request.
    /// Zero while any request is active. Observation does not reset this duration.
    /// </summary>
    public TimeSpan IdleDuration { get; }
}
