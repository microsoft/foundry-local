// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.OpenAI;

using System.IO;
using System.Text;
using System.Text.Json;

using Betalgo.Ranul.OpenAI.ObjectModels.RequestModels;
using Betalgo.Ranul.OpenAI.ObjectModels.ResponseModels;

using Microsoft.AI.Foundry.Local;
using Microsoft.AI.Foundry.Local.Detail;

using Microsoft.Extensions.Logging;

internal record AudioTranscriptionCreateRequestExtended : AudioCreateTranscriptionRequest
{
    internal static AudioTranscriptionCreateRequestExtended FromUserInput(string modelId,
                                                                      string audioFilePath,
#pragma warning disable CS0618 // OpenAIAudioClient is obsolete
                                                                      OpenAIAudioClient.AudioSettings settings)
#pragma warning restore CS0618
    {
        return new AudioTranscriptionCreateRequestExtended
        {
            Model = modelId,
            FileName = audioFilePath,

            // apply our specific settings
            Language = settings.Language,
            Temperature = settings.Temperature
        };
    }
}
internal static class AudioTranscriptionRequestResponseExtensions
{
    /// <summary>
    /// Serialize using Utf8JsonWriter with OpenAI-spec field names.
    /// Betalgo's AudioCreateTranscriptionRequest lacks [JsonPropertyName] attributes
    /// (it was designed for multipart form-data, not JSON body), so source-gen
    /// serialization produces PascalCase keys that the C++ side can't parse.
    /// </summary>
    internal static string ToJson(this AudioTranscriptionCreateRequestExtended request)
    {
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream))
        {
            writer.WriteStartObject();
            writer.WriteString("model", request.Model);
            writer.WriteString("filename", request.FileName);

            if (request.Language != null)
            {
                writer.WriteString("language", request.Language);
            }

            if (request.Temperature.HasValue)
            {
                writer.WriteNumber("temperature", request.Temperature.Value);
            }

            writer.WriteEndObject();
        }

        return Encoding.UTF8.GetString(stream.ToArray());
    }

    internal static AudioCreateTranscriptionResponse ToAudioTranscription(this string responseData, ILogger logger)
    {
        var typeInfo = JsonSerializationContext.Default.AudioCreateTranscriptionResponse;
        var response = JsonSerializer.Deserialize(responseData, typeInfo);
        if (response == null)
        {
            logger.LogError("Failed to deserialize AudioCreateTranscriptionResponse. Json={Data}", responseData);
            throw new FoundryLocalException("Failed to deserialize AudioCreateTranscriptionResponse");
        }

        return response;
    }
}
