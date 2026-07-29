// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Text.Json;

using Betalgo.Ranul.OpenAI.ObjectModels.ResponseModels;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.AI.Foundry.Local.OpenAI;
using Microsoft.Extensions.Logging;

using NativeModel = Microsoft.AI.Foundry.Local.Detail.Native.Model;

/// <summary>
/// Audio Client that uses the OpenAI API.
/// Implemented using Betalgo.Ranul.OpenAI SDK types.
/// </summary>
[System.Obsolete(
    "OpenAIAudioClient is deprecated and will be removed at the end of 2026. " +
    "Use AudioSession instead. OpenAI types remain supported for the web-server path.",
    error: false)]
public class OpenAIAudioClient
{
    private readonly string _modelId;
    private readonly NativeModel _nativeModel;
    private readonly ILogger _logger;

    internal OpenAIAudioClient(string modelId, NativeModel nativeModel)
    {
        _modelId = modelId;
        _nativeModel = nativeModel;
        _logger = FoundryLocalManager.Instance.Logger;
    }

    /// <summary>
    /// Settings that are supported by Foundry Local
    /// </summary>
    public record AudioSettings
    {
        public string? Language { get; set; }
        public float? Temperature { get; set; }
    }

    /// <summary>
    /// Settings to use for audio transcription using this client.
    /// </summary>
    public AudioSettings Settings { get; } = new();

    /// <summary>
    /// Transcribe audio from a file.
    /// </summary>
    /// <param name="audioFilePath">
    /// Path to file containing audio recording.
    /// Supported formats: mp3
    /// </param>
    /// <param name="ct">Optional cancellation token.</param>
    /// <returns>Transcription response.</returns>
    public async Task<AudioCreateTranscriptionResponse> TranscribeAudioAsync(string audioFilePath,
                                                                             CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(() => TranscribeAudioImplAsync(audioFilePath, ct),
                                                          "Error during audio transcription.", _logger)
                                                         .ConfigureAwait(false);
    }

    /// <summary>
    /// Transcribe audio from a file with streamed output.
    /// </summary>
    /// <param name="audioFilePath">
    /// Path to file containing audio recording.
    /// Supported formats: mp3
    /// </param>
    /// <param name="ct">Cancellation token.</param>
    /// <returns>An asynchronous enumerable of transcription responses.</returns>
    public IAsyncEnumerable<AudioCreateTranscriptionResponse> TranscribeAudioStreamingAsync(
        string audioFilePath, CancellationToken ct)
    {
        return Utils.WrapStreamingExceptions(
            TranscribeAudioStreamingImplAsync(audioFilePath, ct),
            "Error during streaming audio transcription.", _logger, ct);
    }

    /// <summary>
    /// Create a real-time streaming transcription session.
    /// Audio data is pushed in as PCM chunks and transcription results are returned as an async stream.
    /// </summary>
    /// <returns>A streaming session that must be disposed when done.</returns>
    public LiveAudioTranscriptionSession CreateLiveTranscriptionSession()
    {
        return new LiveAudioTranscriptionSession(_modelId, _nativeModel);
    }

    private Task<AudioCreateTranscriptionResponse> TranscribeAudioImplAsync(string audioFilePath,
                                                                             CancellationToken? ct)
    {
        var requestJson = AudioTranscriptionCreateRequestExtended
            .FromUserInput(_modelId, audioFilePath, Settings)
            .ToJson();

        return NativeRequestRunner.RunAsync(
            _nativeModel,
            requestJson,
            json => JsonSerializer.Deserialize(json, JsonSerializationContext.Default.AudioCreateTranscriptionResponse)
                    ?? throw new FoundryLocalException("Failed to deserialize audio transcription response."),
            _logger,
            ct);
    }

    private IAsyncEnumerable<AudioCreateTranscriptionResponse> TranscribeAudioStreamingImplAsync(
        string audioFilePath, CancellationToken ct)
    {
        var requestJson = AudioTranscriptionCreateRequestExtended
            .FromUserInput(_modelId, audioFilePath, Settings)
            .ToJson();

        return NativeRequestRunner.RunStreamingAsync<AudioCreateTranscriptionResponse>(
            _nativeModel,
            requestJson,
            json => JsonSerializer.Deserialize(json, JsonSerializationContext.Default.AudioCreateTranscriptionResponse),
            _logger,
            "Error processing streaming audio transcription callback data.",
            "Error executing streaming audio transcription.",
            ct);
    }
}
