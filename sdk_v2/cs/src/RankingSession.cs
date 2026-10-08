// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using Microsoft.AI.Foundry.Local.Detail;

/// <summary>Typed session for models whose task is <c>text-ranking</c>.</summary>
public sealed class RankingSession : NonGenerativeSession
{
    /// <summary>Creates a ranking session for a cached text-ranking model.</summary>
    public RankingSession(IModel model) : base(ValidateTask(model))
    {
    }

    internal RankingSession(Func<string, CancellationToken, Task<string>> testTransport)
        : base(testTransport)
    {
    }

    /// <summary>Ranks the request's candidate answers.</summary>
    public Task<RankingResult> RankAsync(RankingRequest request, CancellationToken ct = default)
        => ProcessAsync(
            request,
            JsonSerializationContext.Default.RankingRequest,
            JsonSerializationContext.Default.RankingResult,
            ct);

    private static IModel ValidateTask(IModel model)
    {
        Detail.Throw.IfNull(model);
        if (model.Info.Task != "text-ranking")
        {
            throw new ArgumentException(
                $"RankingSession requires a model with task 'text-ranking', but got '{model.Info.Task}'.",
                nameof(model));
        }

        return model;
    }
}
