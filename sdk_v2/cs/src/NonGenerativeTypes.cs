// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Text.Json;
using System.Text.Json.Serialization;

/// <summary>A request matching the JSON contract accepted by <c>POST /v1/rank</c>.</summary>
public sealed record RankingRequest
{
    [JsonPropertyName("context")]
    public JsonElement? Context { get; init; }

    [JsonPropertyName("question")]
    public string Question { get; init; } = string.Empty;

    [JsonPropertyName("answers")]
    public required IReadOnlyList<string> Answers { get; init; }

    [JsonPropertyName("model")]
    public string Model { get; init; } = "clm";

    [JsonPropertyName("temperature")]
    public float Temperature { get; init; } = 1.0f;
}

/// <summary>A ranked candidate returned by a ranking model.</summary>
public sealed record RankedCandidate
{
    [JsonPropertyName("rank")]
    public int Rank { get; init; }

    [JsonPropertyName("candidate")]
    public required string Candidate { get; init; }

    [JsonPropertyName("prob")]
    public double Probability { get; init; }
}

/// <summary>A response matching the JSON contract returned by <c>POST /v1/rank</c>.</summary>
public sealed record RankingResult
{
    [JsonPropertyName("model")]
    public required string Model { get; init; }

    [JsonPropertyName("ranked")]
    public required IReadOnlyList<RankedCandidate> Ranked { get; init; }
}

/// <summary>A typed question in a <see cref="DecisionRequest"/>.</summary>
public sealed record DecisionQuestion
{
    /// <summary>The question kind: <c>noul</c>, <c>choice</c>, or <c>score</c>.</summary>
    [JsonPropertyName("type")]
    public required string Type { get; init; }

    [JsonPropertyName("criteria")]
    public JsonElement? Criteria { get; init; }

    [JsonPropertyName("instructions")]
    public JsonElement? Instructions { get; init; }
}

/// <summary>A request matching the JSON contract accepted by <c>POST /v1/systemone</c>.</summary>
public sealed record DecisionRequest
{
    [JsonPropertyName("state")]
    public JsonElement? State { get; init; }

    [JsonPropertyName("questions")]
    public required IReadOnlyDictionary<string, DecisionQuestion> Questions { get; init; }

    [JsonPropertyName("model")]
    public string Model { get; init; } = "kev";

    [JsonPropertyName("temperature")]
    public float Temperature { get; init; } = 1.0f;
}

/// <summary>A single typed-decision answer.</summary>
public sealed record DecisionAnswer
{
    [JsonPropertyName("type")]
    public required string Type { get; init; }

    [JsonPropertyName("noul")]
    public double? Noul { get; init; }

    [JsonPropertyName("choice")]
    public string? Choice { get; init; }

    [JsonPropertyName("score")]
    public double? Score { get; init; }

    [JsonPropertyName("confidence")]
    public double? Confidence { get; init; }

    [JsonPropertyName("probabilities")]
    public IReadOnlyDictionary<string, double>? Probabilities { get; init; }

    [JsonPropertyName("legend")]
    public IReadOnlyDictionary<string, string>? Legend { get; init; }
}

/// <summary>Usage information returned by a typed-decision model.</summary>
public sealed record DecisionUsage
{
    [JsonPropertyName("billing_units")]
    public long BillingUnits { get; init; }
}

/// <summary>A response matching the JSON contract returned by <c>POST /v1/systemone</c>.</summary>
public sealed record DecisionResult
{
    [JsonPropertyName("model")]
    public required string Model { get; init; }

    [JsonPropertyName("answers")]
    public required IReadOnlyDictionary<string, DecisionAnswer> Answers { get; init; }

    [JsonPropertyName("usage")]
    public required DecisionUsage Usage { get; init; }
}
