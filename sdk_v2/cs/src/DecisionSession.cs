// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Text.Json;

using Microsoft.AI.Foundry.Local.Detail;

/// <summary>Typed session for models whose task is <c>typed-decision</c>.</summary>
public sealed class DecisionSession : NonGenerativeSession
{
    /// <summary>
    /// Creates a decision session for a loaded typed-decision model. Dispose the session before unloading the model.
    /// </summary>
    public DecisionSession(IModel model) : base(ValidateTask(model))
    {
    }

    internal DecisionSession(Func<string, CancellationToken, Task<string>> testTransport)
        : base(testTransport)
    {
    }

    /// <summary>Evaluates the request's typed questions.</summary>
    public async Task<DecisionResult> DecideAsync(DecisionRequest request, CancellationToken ct = default)
    {
        ValidateRequest(request);
        var result = await ProcessAsync(
            request,
            JsonSerializationContext.Default.DecisionRequest,
            JsonSerializationContext.Default.DecisionResult,
            ct).ConfigureAwait(false);
        ValidateResult(request, result);
        return result;
    }

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

    private static void ValidateRequest(DecisionRequest request)
    {
        Detail.Throw.IfNull(request);
        if (request.Questions == null || request.Questions.Count == 0)
        {
            throw new ArgumentException("Decision questions must be a non-empty collection.", nameof(request));
        }
        if (string.IsNullOrWhiteSpace(request.Model))
        {
            throw new ArgumentException("Decision model must be a non-empty string.", nameof(request));
        }
        if (!float.IsFinite(request.Temperature) || request.Temperature != 1.0f)
        {
            throw new ArgumentOutOfRangeException(
                nameof(request), "Decision temperature must be finite and exactly 1.0.");
        }

        NonGenerativeValidation.ValidateJson(request.State, "Decision state");
        foreach (var (questionId, question) in request.Questions)
        {
            if (question == null)
            {
                throw new ArgumentException($"Decision question '{questionId}' is required.", nameof(request));
            }
            if (question.Type is not ("noul" or "choice" or "score"))
            {
                throw new ArgumentException(
                    $"Decision question '{questionId}' type must be 'noul', 'choice', or 'score'.",
                    nameof(request));
            }

            NonGenerativeValidation.ValidateJson(
                question.Instructions, $"Decision question '{questionId}' instructions");
            ValidateCriteria(questionId, question);
        }
    }

    private static void ValidateCriteria(string questionId, DecisionQuestion question)
    {
        if (question.Type == "noul")
        {
            if (question.Criteria != null)
            {
                throw new ArgumentException(
                    $"Decision question '{questionId}' of type 'noul' must not specify criteria.");
            }
            return;
        }

        if (question.Criteria is not JsonElement criteria)
        {
            throw new ArgumentException(
                $"Decision question '{questionId}' of type '{question.Type}' requires criteria.");
        }
        var expectedKind = question.Type == "choice" ? JsonValueKind.Object : JsonValueKind.Array;
        if (criteria.ValueKind != expectedKind || criteria.GetArrayOrObjectLength() is < 1 or > 255)
        {
            throw new ArgumentException(
                $"Decision question '{questionId}' {question.Type} criteria must be a non-empty " +
                $"{expectedKind.ToString().ToLowerInvariant()} with at most 255 entries.");
        }
        NonGenerativeValidation.ValidateJson(criteria, $"Decision question '{questionId}' criteria");
    }

    private static void ValidateResult(DecisionRequest request, DecisionResult result)
    {
        if (string.IsNullOrWhiteSpace(result.Model) || result.Answers == null || result.Usage == null ||
            result.Usage.BillingUnits < 0)
        {
            throw InvalidResult("model, answers, and non-negative usage are required");
        }
        if (result.Answers.Count != request.Questions.Count ||
            result.Answers.Keys.Any(key => !request.Questions.ContainsKey(key)))
        {
            throw InvalidResult("answer keys must exactly match question keys");
        }

        foreach (var (questionId, answer) in result.Answers)
        {
            if (answer == null || answer.Type != request.Questions[questionId].Type)
            {
                throw InvalidResult($"answer '{questionId}' type does not match its question");
            }
            ValidateAnswer(questionId, request.Questions[questionId], answer);
        }
    }

    private static void ValidateAnswer(
        string questionId,
        DecisionQuestion question,
        DecisionAnswer answer)
    {
        var hasNoul = answer.Noul.HasValue;
        var hasChoice = answer.Choice != null;
        var hasScore = answer.Score.HasValue;
        if ((answer.Type == "noul" && (!hasNoul || hasChoice || hasScore)) ||
            (answer.Type == "choice" && (hasNoul || !hasChoice || hasScore)) ||
            (answer.Type == "score" && (hasNoul || hasChoice || !hasScore)))
        {
            throw InvalidResult($"answer '{questionId}' must contain only its type-specific value");
        }
        if ((answer.Noul is double noul && !double.IsFinite(noul)) ||
            (answer.Score is double score && !double.IsFinite(score)) ||
            (answer.Confidence is double confidence &&
             (!double.IsFinite(confidence) || confidence < 0 || confidence > 1)) ||
            (answer.Probabilities != null &&
             answer.Probabilities.Any(pair =>
                 string.IsNullOrEmpty(pair.Key) || !double.IsFinite(pair.Value) ||
                 pair.Value < 0 || pair.Value > 1)) ||
            (answer.Legend != null &&
             answer.Legend.Any(pair => string.IsNullOrEmpty(pair.Key) || pair.Value == null)))
        {
            throw InvalidResult($"answer '{questionId}' contains invalid numeric or metadata values");
        }

        if (answer.Noul is double noulValue && (noulValue < 0 || noulValue > 1))
        {
            throw InvalidResult($"answer '{questionId}' noul value must be in [0, 1]");
        }
        if (answer.Type == "choice" &&
            question.Criteria is JsonElement choiceCriteria &&
            !choiceCriteria.TryGetProperty(answer.Choice!, out _))
        {
            throw InvalidResult($"answer '{questionId}' choice must match a criteria key");
        }
        if (answer.Score is double scoreValue &&
            question.Criteria is JsonElement scoreCriteria &&
            (scoreValue < 0 || scoreValue > scoreCriteria.GetArrayLength() - 1))
        {
            throw InvalidResult($"answer '{questionId}' score is outside its criteria range");
        }
    }

    private static FoundryLocalException InvalidResult(string detail) =>
        new($"Decision session returned an invalid result: {detail}.");
}
