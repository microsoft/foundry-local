// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using Microsoft.AI.Foundry.Local.Detail;

/// <summary>Typed session for models whose task is <c>typed-decision</c>.</summary>
public sealed class DecisionSession : NonGenerativeSession
{
    /// <summary>Creates a decision session for a cached typed-decision model.</summary>
    public DecisionSession(IModel model) : base(ValidateTask(model))
    {
    }

    internal DecisionSession(Func<string, CancellationToken, Task<string>> testTransport)
        : base(testTransport)
    {
    }

    /// <summary>Evaluates the request's typed questions.</summary>
    public Task<DecisionResult> DecideAsync(DecisionRequest request, CancellationToken ct = default)
        => ProcessAsync(
            request,
            JsonSerializationContext.Default.DecisionRequest,
            JsonSerializationContext.Default.DecisionResult,
            ct);

    private static IModel ValidateTask(IModel model)
    {
        Detail.Throw.IfNull(model);
        if (model.Info.Task != "typed-decision")
        {
            throw new ArgumentException(
                $"DecisionSession requires a model with task 'typed-decision', but got '{model.Info.Task}'.",
                nameof(model));
        }

        return model;
    }
}
