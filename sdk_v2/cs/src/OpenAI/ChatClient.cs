// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local;

using System.Collections.Generic;
using System.Text.Json;

using Betalgo.Ranul.OpenAI.ObjectModels.RequestModels;
using Betalgo.Ranul.OpenAI.ObjectModels.ResponseModels;

using Microsoft.AI.Foundry.Local.Detail;
using Microsoft.AI.Foundry.Local.OpenAI;
using Microsoft.Extensions.Logging;

using NativeModel = Microsoft.AI.Foundry.Local.Detail.Native.Model;
using OpenAIChatMessage = Betalgo.Ranul.OpenAI.ObjectModels.RequestModels.ChatMessage;
using OpenAIToolChoice = Betalgo.Ranul.OpenAI.ObjectModels.RequestModels.ToolChoice;

/// <summary>
/// Chat Client that uses the OpenAI API.
/// Implemented using Betalgo.Ranul.OpenAI SDK types.
/// </summary>
public class OpenAIChatClient
{
    private readonly string _modelId;
    private readonly NativeModel _nativeModel;
    private readonly ILogger _logger;

    internal OpenAIChatClient(string modelId, NativeModel nativeModel)
    {
        _modelId = modelId;
        _nativeModel = nativeModel;
        _logger = FoundryLocalManager.Instance.Logger;
    }

    /// <summary>
    /// Settings that are supported by Foundry Local
    /// </summary>
    public record ChatSettings
    {
        public float? FrequencyPenalty { get; set; }
        public int? MaxTokens { get; set; }
        public int? N { get; set; }
        public float? Temperature { get; set; }
        public float? PresencePenalty { get; set; }
        public int? RandomSeed { get; set; }
        public int? TopK { get; set; }
        public float? TopP { get; set; }
        // Settings for tool calling and structured outputs
        public ResponseFormatExtended? ResponseFormat { get; set; }

        public OpenAIToolChoice? ToolChoice { get; set; }
    }

    /// <summary>
    /// Settings to use for chat completions using this client.
    /// </summary>
    public ChatSettings Settings { get; } = new();

    /// <summary>
    /// Execute a chat completion request.
    /// </summary>
    public Task<ChatCompletionCreateResponse> CompleteChatAsync(IEnumerable<OpenAIChatMessage> messages,
                                                                CancellationToken? ct = null)
    {
        return CompleteChatAsync(messages: messages, tools: null, ct: ct);
    }

    /// <summary>
    /// Execute a chat completion request.
    /// </summary>
    public async Task<ChatCompletionCreateResponse> CompleteChatAsync(IEnumerable<OpenAIChatMessage> messages,
                                                                      IEnumerable<ToolDefinition>? tools,
                                                                      CancellationToken? ct = null)
    {
        return await Utils.CallWithExceptionHandlingAsync(
            () => CompleteChatImplAsync(messages, tools, ct),
            "Error during chat completion.", _logger).ConfigureAwait(false);
    }

    /// <summary>
    /// Execute a chat completion request with streamed output.
    /// </summary>
    public IAsyncEnumerable<ChatCompletionCreateResponse> CompleteChatStreamingAsync(IEnumerable<OpenAIChatMessage> messages,
                                                                                     CancellationToken ct)
    {
        return CompleteChatStreamingAsync(messages: messages, tools: null, ct: ct);
    }

    /// <summary>
    /// Execute a chat completion request with streamed output.
    /// </summary>
    public IAsyncEnumerable<ChatCompletionCreateResponse> CompleteChatStreamingAsync(IEnumerable<OpenAIChatMessage> messages,
                                                                                     IEnumerable<ToolDefinition>? tools,
                                                                                     CancellationToken ct)
    {
        return Utils.WrapStreamingExceptions(
            ChatStreamingImplAsync(messages, tools, ct),
            "Error during streaming chat completion.", _logger, ct);
    }

    private Task<ChatCompletionCreateResponse> CompleteChatImplAsync(IEnumerable<OpenAIChatMessage> messages,
                                                                      IEnumerable<ToolDefinition>? tools,
                                                                      CancellationToken? ct)
    {
        var chatRequestJson = ChatCompletionCreateRequestExtended
            .FromUserInput(_modelId, messages, tools, Settings, stream: false)
            .ToJson();

        return NativeRequestRunner.RunAsync(
            _nativeModel,
            chatRequestJson,
            json => JsonSerializer.Deserialize(json, JsonSerializationContext.Default.ChatCompletionCreateResponse)
                    ?? throw new FoundryLocalException("Failed to deserialize chat completion response."),
            _logger,
            ct);
    }

    private IAsyncEnumerable<ChatCompletionCreateResponse> ChatStreamingImplAsync(
        IEnumerable<OpenAIChatMessage> messages,
        IEnumerable<ToolDefinition>? tools,
        CancellationToken ct)
    {
        var chatRequestJson = ChatCompletionCreateRequestExtended
            .FromUserInput(_modelId, messages, tools, Settings, stream: true)
            .ToJson();

        return NativeRequestRunner.RunStreamingAsync<ChatCompletionCreateResponse>(
            _nativeModel,
            chatRequestJson,
            json => JsonSerializer.Deserialize(json, JsonSerializationContext.Default.ChatCompletionCreateResponse),
            _logger,
            "Error processing streaming chat completion callback data.",
            "Error executing streaming chat completion.",
            ct);
    }
}
