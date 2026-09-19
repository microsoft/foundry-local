// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

/// <summary>
/// Exact token-budget preflight result for a request captured in a chat session's current state.
/// </summary>
public sealed class RequestPreflightResult
{
    internal RequestPreflightResult(
        long promptTokens,
        long outputReserveTokens,
        long requiredTokens,
        long contextLimitTokens,
        bool fits,
        long deficitTokens)
    {
        PromptTokens = promptTokens;
        OutputReserveTokens = outputReserveTokens;
        RequiredTokens = requiredTokens;
        ContextLimitTokens = contextLimitTokens;
        Fits = fits;
        DeficitTokens = deficitTokens;
    }

    public long PromptTokens { get; }

    public long OutputReserveTokens { get; }

    public long RequiredTokens { get; }

    public long ContextLimitTokens { get; }

    public bool Fits { get; }

    public long DeficitTokens { get; }
}
