// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.OpenAI;

using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;

using Betalgo.Ranul.OpenAI.ObjectModels.RequestModels;
using Betalgo.Ranul.OpenAI.ObjectModels.ResponseModels;

using Microsoft.AI.Foundry.Local;
using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.Extensions.Logging;

using OpenAIChatMessage = Betalgo.Ranul.OpenAI.ObjectModels.RequestModels.ChatMessage;

// https://platform.openai.com/docs/api-reference/chat/create
// Using the Betalgo ChatCompletionCreateRequest and extending with the `metadata` field for additional parameters
// which is part of the OpenAI spec but for some reason not part of the Betalgo request object.
internal class ChatCompletionCreateRequestExtended : ChatCompletionCreateRequest
{
    // Valid entries:
    // int top_k
    // int random_seed
    [JsonPropertyName("metadata")]
    public Dictionary<string, string>? Metadata { get; set; }

    [JsonPropertyName("response_format")]
    public new ResponseFormatExtended? ResponseFormat { get; set; }

    internal static ChatCompletionCreateRequestExtended FromUserInput(string modelId,
                                                                      IEnumerable<OpenAIChatMessage> messages,
                                                                      IEnumerable<ToolDefinition>? tools,
                                                                      OpenAIChatClient.ChatSettings settings,
                                                                      bool stream)
    {
        var request = new ChatCompletionCreateRequestExtended
        {
            Model = modelId,
            Messages = messages.ToList(),
            Tools = tools?.ToList(),
            // Apply our specific settings
            FrequencyPenalty = settings.FrequencyPenalty,
            MaxTokens = settings.MaxTokens,
            N = settings.N,
            Temperature = settings.Temperature,
            PresencePenalty = settings.PresencePenalty,
            Stream = stream,
            TopP = settings.TopP,
            // Apply tool calling and structured output settings
            ResponseFormat = settings.ResponseFormat,
            ToolChoice = settings.ToolChoice
        };

        var metadata = new Dictionary<string, string>();

        if (settings.TopK.HasValue)
        {
            metadata["top_k"] = settings.TopK.Value.ToString(CultureInfo.InvariantCulture);
        }

        if (settings.RandomSeed.HasValue)
        {
            metadata["random_seed"] = settings.RandomSeed.Value.ToString(CultureInfo.InvariantCulture);
        }

        if (metadata.Count > 0)
        {
            request.Metadata = metadata;
        }


        return request;
    }
}

internal static class ChatCompletionsRequestResponseExtensions
{
    internal static string ToJson(this ChatCompletionCreateRequestExtended request)
    {
        return JsonSerializer.Serialize(request, JsonSerializationContext.Default.ChatCompletionCreateRequestExtended);
    }

    internal static ChatCompletionCreateResponse ToChatCompletion(this string responseData, ILogger logger)
    {
        var output = JsonSerializer.Deserialize(responseData, JsonSerializationContext.Default.ChatCompletionCreateResponse);
        if (output == null)
        {
            logger.LogError("Failed to deserialize chat completion response: {ResponseData}", responseData);
            throw new JsonException("Failed to deserialize ChatCompletion");
        }

        return output;
    }
}
