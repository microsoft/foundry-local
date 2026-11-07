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
        var questions = SnapshotQuestions(request.Questions);
        var result = await ProcessAsync(
            request,
            JsonSerializationContext.Default.DecisionRequest,
            JsonSerializationContext.Default.DecisionResult,
            ct).ConfigureAwait(false);
        ValidateResult(questions, result);
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
        if (request.Model != null && string.IsNullOrWhiteSpace(request.Model))
        {
            throw new ArgumentException("Decision model must be a non-empty string.", nameof(request));
        }
        if (request.Temperature is float temperature &&
            (!NonGenerativeValidation.IsFinite(temperature) || temperature != 1.0f))
        {
            throw new ArgumentOutOfRangeException(
                nameof(request), "Decision temperature must be finite and exactly 1.0.");
        }

        NonGenerativeValidation.ValidateJson(request.State, "Decision state");
        foreach (var entry in request.Questions)
        {
            var questionId = entry.Key;
            var question = entry.Value;
            if (question == null)
            {
                throw new ArgumentException($"Decision question '{questionId}' is required.", nameof(request));
            }
            if (question.Type != DecisionQuestionKind.Noul &&
                question.Type != DecisionQuestionKind.Choice &&
                question.Type != DecisionQuestionKind.Score)
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
        if (question.Type == DecisionQuestionKind.Noul)
        {
            if (question.Criteria is JsonElement noulCriteria &&
                noulCriteria.ValueKind != JsonValueKind.Null)
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
        var expectedKind = question.Type == DecisionQuestionKind.Choice
            ? JsonValueKind.Object
            : JsonValueKind.Array;
        if (criteria.ValueKind != expectedKind || criteria.GetArrayOrObjectLength() is < 1 or > 255)
        {
            throw new ArgumentException(
                $"Decision question '{questionId}' {question.Type} criteria must be a non-empty " +
                $"{expectedKind.ToString().ToLowerInvariant()} with at most 255 entries.");
        }
        NonGenerativeValidation.ValidateJson(criteria, $"Decision question '{questionId}' criteria");
    }

    private static Dictionary<string, QuestionSnapshot> SnapshotQuestions(
        IReadOnlyDictionary<string, DecisionQuestion> questions)
    {
        var snapshot = new Dictionary<string, QuestionSnapshot>(questions.Count, StringComparer.Ordinal);
        foreach (var entry in questions)
        {
            var question = entry.Value;
            HashSet<string>? choiceKeys = null;
            int? maximumScore = null;
            if (question.Type == DecisionQuestionKind.Choice)
            {
                choiceKeys = new HashSet<string>(StringComparer.Ordinal);
                using var properties = question.Criteria!.Value.EnumerateObject();
                while (properties.MoveNext())
                {
                    choiceKeys.Add(properties.Current.Name);
                }
            }
            else if (question.Type == DecisionQuestionKind.Score)
            {
                maximumScore = question.Criteria!.Value.GetArrayLength() - 1;
            }

            snapshot.Add(entry.Key, new QuestionSnapshot(question.Type, choiceKeys, maximumScore));
        }

        return snapshot;
    }

    private static void ValidateResult(
        Dictionary<string, QuestionSnapshot> questions,
        DecisionResult result)
    {
        if (string.IsNullOrWhiteSpace(result.Model) || result.Answers == null || result.Usage == null ||
            result.Usage.BillingUnits < 0)
        {
            throw InvalidResult("model, answers, and non-negative usage are required");
        }
        if (result.Answers.Count != questions.Count ||
            result.Answers.Keys.Any(key => !questions.ContainsKey(key)))
        {
            throw InvalidResult("answer keys must exactly match question keys");
        }

        foreach (var entry in result.Answers)
        {
            var questionId = entry.Key;
            var answer = entry.Value;
            var question = questions[questionId];
            if (answer == null || answer.Type != question.Type)
            {
                throw InvalidResult($"answer '{questionId}' type does not match its question");
            }
            ValidateAnswer(questionId, question, answer);
        }
    }

    private static void ValidateAnswer(
        string questionId,
        QuestionSnapshot question,
        DecisionAnswer answer)
    {
        var hasNoul = answer.Noul.HasValue;
        var hasChoice = answer.Choice != null;
        var hasScore = answer.Score.HasValue;
        if ((answer.Type == DecisionQuestionKind.Noul && (!hasNoul || hasChoice || hasScore)) ||
            (answer.Type == DecisionQuestionKind.Choice && (hasNoul || !hasChoice || hasScore)) ||
            (answer.Type == DecisionQuestionKind.Score && (hasNoul || hasChoice || !hasScore)))
        {
            throw InvalidResult($"answer '{questionId}' must contain only its type-specific value");
        }
        if ((answer.Noul is double noul && !NonGenerativeValidation.IsFinite(noul)) ||
            (answer.Score is double score && !NonGenerativeValidation.IsFinite(score)) ||
            (answer.Confidence is double confidence &&
             (!NonGenerativeValidation.IsFinite(confidence) || confidence < 0 || confidence > 1)) ||
            (answer.Probabilities != null &&
             answer.Probabilities.Any(pair =>
                 string.IsNullOrEmpty(pair.Key) || !NonGenerativeValidation.IsFinite(pair.Value) ||
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
        if (answer.Type == DecisionQuestionKind.Choice &&
            !question.ChoiceKeys!.Contains(answer.Choice!))
        {
            throw InvalidResult($"answer '{questionId}' choice must match a criteria key");
        }
        if (answer.Score is double scoreValue &&
            (scoreValue < 0 || scoreValue > question.MaximumScore!.Value))
        {
            throw InvalidResult($"answer '{questionId}' score is outside its criteria range");
        }
    }

    private static FoundryLocalException InvalidResult(string detail) =>
        new($"Decision session returned an invalid result: {detail}.");

    private sealed record QuestionSnapshot(
        string Type,
        HashSet<string>? ChoiceKeys,
        int? MaximumScore);
}
