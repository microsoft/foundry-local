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
    /// <summary>
    /// Creates a ranking session for a loaded text-ranking model. Dispose the session before unloading the model.
    /// </summary>
    public RankingSession(IModel model) : base(ValidateTask(model))
    {
    }

    internal RankingSession(Func<string, CancellationToken, Task<string>> testTransport)
        : base(testTransport)
    {
    }

    /// <summary>Ranks the request's candidate answers.</summary>
    public async Task<RankingResult> RankAsync(RankingRequest request, CancellationToken ct = default)
    {
        ValidateRequest(request);
        var answers = request.Answers.ToArray();
        var result = await ProcessAsync(
            request,
            NonGenerativeJsonSerializationContext.Default.RankingRequest,
            NonGenerativeJsonSerializationContext.Default.RankingResult,
            ct).ConfigureAwait(false);
        ValidateResult(answers, result);
        return result;
    }

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

    private static void ValidateRequest(RankingRequest request)
    {
        Detail.Throw.IfNull(request);
        if (request.Question == null)
        {
            throw new ArgumentException("Ranking question must be a string.", nameof(request));
        }
        if (request.Answers == null || request.Answers.Count == 0)
        {
            throw new ArgumentException("Ranking answers must be a non-empty collection.", nameof(request));
        }
        for (var index = 0; index < request.Answers.Count; index++)
        {
            var answer = request.Answers[index];
            if (answer == null || answer.Length == 0 || HasEmbeddedNul(answer))
            {
                throw new ArgumentException(
                    "Ranking answers must contain only non-empty strings without embedded NUL characters.",
                    nameof(request));
            }
        }
        if (request.Model != null && string.IsNullOrWhiteSpace(request.Model))
        {
            throw new ArgumentException("Ranking model must be a non-empty string.", nameof(request));
        }
        if (request.Temperature is float temperature &&
            (!NonGenerativeValidation.IsFinite(temperature) || temperature <= 0 || temperature > 100))
        {
            throw new ArgumentOutOfRangeException(
                nameof(request), "Ranking temperature must be finite and in the range (0, 100].");
        }
        NonGenerativeValidation.ValidateJson(request.Context, "Ranking context");
    }

    private static void ValidateResult(string[] answers, RankingResult result)
    {
        if (string.IsNullOrWhiteSpace(result.Model) ||
            result.Ranked == null ||
            result.Ranked.Count != answers.Length)
        {
            throw InvalidResult("model and one ranked item per submitted answer are required");
        }

        var ranks = new bool[answers.Length];
        var indexes = new bool[answers.Length];
        foreach (var item in result.Ranked)
        {
            if (item == null ||
                item.Rank < 1 || item.Rank > answers.Length || ranks[item.Rank - 1] ||
                item.Index < 0 || item.Index >= answers.Length || indexes[item.Index] ||
                !MatchesIndex(item.Id, item.Index) ||
                item.Candidate != answers[item.Index] ||
                !NonGenerativeValidation.IsFinite(item.Probability) ||
                item.Probability < 0 || item.Probability > 1)
            {
                throw InvalidResult(
                    "ranks and indexes must be unique and in range, candidates must match their indexes, " +
                    "IDs must equal indexes, and probabilities must be finite values in [0, 1]");
            }
            ranks[item.Rank - 1] = true;
            indexes[item.Index] = true;
        }
    }

    private static bool MatchesIndex(string id, int index)
    {
        var value = index;
        var digits = 1;
        while (value >= 10)
        {
            value /= 10;
            digits++;
        }
        if (id == null || id.Length != digits)
        {
            return false;
        }

        value = index;
        for (var position = digits - 1; position >= 0; position--)
        {
            if (id[position] != '0' + value % 10)
            {
                return false;
            }
            value /= 10;
        }
        return true;
    }

    private static bool HasEmbeddedNul(string value)
    {
        foreach (var character in value)
        {
            if (character == '\0')
            {
                return true;
            }
        }
        return false;
    }

    private static FoundryLocalException InvalidResult(string detail) =>
        new($"Ranking session returned an invalid result: {detail}.");
}
