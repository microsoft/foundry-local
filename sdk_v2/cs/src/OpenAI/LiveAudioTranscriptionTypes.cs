// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.OpenAI;

using System.Text.Json;
using System.Text.Json.Serialization;

using Betalgo.Ranul.OpenAI.ObjectModels.RealtimeModels;

using Microsoft.AI.Foundry.Local;
using Microsoft.AI.Foundry.Local.Detail;

/// <summary>
/// Transcription result for real-time audio streaming sessions.
/// Extends the OpenAI Realtime API's <see cref="ConversationItem"/> so that
/// customers access text via <c>result.Content[0].Text</c> or
/// <c>result.Content[0].Transcript</c>, ensuring forward compatibility
/// when the transport layer moves to WebSocket.
/// </summary>
public class LiveAudioTranscriptionResponse : ConversationItem
{
    /// <summary>
    /// Whether this is a final or partial (interim) result.
    /// - Nemotron models always return <c>true</c> (every result is final).
    /// - Other models (e.g., Azure Embedded) may return <c>false</c> for interim
    ///   hypotheses that will be replaced by a subsequent final result.
    /// </summary>
    [JsonPropertyName("is_final")]
    public bool IsFinal { get; init; }

    /// <summary>Start time offset of this segment in the audio stream (seconds).</summary>
    [JsonPropertyName("start_time")]
    public double? StartTime { get; init; }

    /// <summary>End time offset of this segment in the audio stream (seconds).</summary>
    [JsonPropertyName("end_time")]
    public double? EndTime { get; init; }

    internal static LiveAudioTranscriptionResponse FromJson(string json)
    {
        var raw = JsonSerializer.Deserialize(json,
            JsonSerializationContext.Default.LiveAudioTranscriptionRaw)
            ?? throw new FoundryLocalException("Failed to deserialize live audio transcription result");

        return new LiveAudioTranscriptionResponse
        {
            Id = raw.Id ?? string.Empty,
            IsFinal = raw.IsFinal,
            StartTime = raw.StartTime,
            EndTime = raw.EndTime,
            Content =
            [
                new ContentPart
                {
                    Text = raw.Text,
                    Transcript = raw.Text
                }
            ]
        };
    }
}

/// <summary>
/// Internal raw deserialization target matching the Core's JSON format.
/// Mapped to <see cref="LiveAudioTranscriptionResponse"/> in FromJson.
/// </summary>
internal record LiveAudioTranscriptionRaw
{
    [JsonPropertyName("id")]
    public string? Id { get; init; }

    [JsonPropertyName("is_final")]
    public bool IsFinal { get; init; }

    [JsonPropertyName("text")]
    public string Text { get; init; } = string.Empty;

    [JsonPropertyName("start_time")]
    public double? StartTime { get; init; }

    [JsonPropertyName("end_time")]
    public double? EndTime { get; init; }
}

internal record CoreErrorResponse
{
    [JsonPropertyName("code")]
    public string Code { get; init; } = "";

    [JsonPropertyName("message")]
    public string Message { get; init; } = "";

    [JsonPropertyName("isTransient")]
    public bool IsTransient { get; init; }

    /// <summary>
    /// Attempt to parse a native error string as structured JSON.
    /// Returns null if the error is not valid JSON or doesn't match the schema,
    /// which should be treated as a permanent/unknown error.
    /// </summary>
    internal static CoreErrorResponse? TryParse(string errorString)
    {
        try
        {
            return JsonSerializer.Deserialize(errorString,
                JsonSerializationContext.Default.CoreErrorResponse);
        }
        catch
        {
            return null; // unstructured error — treat as permanent
        }
    }
}