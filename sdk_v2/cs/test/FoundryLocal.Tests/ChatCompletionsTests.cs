// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Text;
using System.Threading.Tasks;

using Betalgo.Ranul.OpenAI.ObjectModels.RequestModels;
using Betalgo.Ranul.OpenAI.ObjectModels.ResponseModels;
using Betalgo.Ranul.OpenAI.ObjectModels.SharedModels;

// Disambiguate from Microsoft.AI.Foundry.Local.ToolChoice (typed RequestOptions enum)
// which lives in the parent namespace and would otherwise shadow the Betalgo type here.
using OpenAIToolChoice = Betalgo.Ranul.OpenAI.ObjectModels.RequestModels.ToolChoice;

[SkipUnlessIntegration]
internal sealed class OpenAIChatCompletionsTests
{
    private static IModel? model;

    [Before(Class)]
    public static async Task Setup()
    {
        var manager = FoundryLocalManager.Instance; // initialized by Utils
        var catalog = await manager.GetCatalogAsync();

        // Load the specific cached model variant directly
        var model = await catalog.GetModelVariantAsync("qwen2.5-0.5b-instruct-generic-cpu:4").ConfigureAwait(false);
        await Assert.That(model).IsNotNull();

        await model!.LoadAsync().ConfigureAwait(false);
        await Assert.That(await model.IsLoadedAsync()).IsTrue();

        OpenAIChatCompletionsTests.model = model;
    }

    [Test]
    public async Task DirectChat_NoStreaming_Succeeds()
    {
        var chatClient = await model!.GetChatClientAsync();
        await Assert.That(chatClient).IsNotNull();

        chatClient.Settings.MaxTokens = 500;
        chatClient.Settings.Temperature = 0.0f; // for deterministic results

        List<ChatMessage> messages =
        [
            // System prompt is setup by GenAI
            new ChatMessage { Role = "user", Content = "You are a calculator. Be precise. What is the answer to 7 multiplied by 6?" }
        ];

        var response = await chatClient.CompleteChatAsync(messages).ConfigureAwait(false);

        await Assert.That(response).IsNotNull();
        await Assert.That(response.Choices).IsNotNull().And.IsNotEmpty();
        var message = response.Choices[0].Message;
        await Assert.That(message).IsNotNull();
        await Assert.That(message.Role).IsEqualTo("assistant");
        await Assert.That(message.Content).IsNotNull();
        await Assert.That(message.Content).Contains("42");
        Console.WriteLine($"Response: {message.Content}");

        messages.Add(new ChatMessage { Role = "assistant", Content = message.Content });

        messages.Add(new ChatMessage
        {
            Role = "user",
            Content = "Is the answer a real number?"
        });

        response = await chatClient.CompleteChatAsync(messages).ConfigureAwait(false);
        message = response.Choices[0].Message;
        await Assert.That(message.Content).IsNotNull();
        await Assert.That(message.Content).Contains("Yes");
    }

    [Test]
    public async Task DirectChat_Streaming_Succeeds()
    {
        var chatClient = await model!.GetChatClientAsync();
        await Assert.That(chatClient).IsNotNull();

        chatClient.Settings.MaxTokens = 500;
        chatClient.Settings.Temperature = 0.0f; // for deterministic results

        List<ChatMessage> messages =
        [
            new ChatMessage { Role = "user", Content = "You are a calculator. Be precise. What is the answer to 7 multiplied by 6?" }
        ];

        var updates = chatClient.CompleteChatStreamingAsync(messages, CancellationToken.None).ConfigureAwait(false);

        bool isFirstChunk = true;
        bool containsFinishReasonStop = false;
        StringBuilder responseMessage = new();

        var validateResponse = async (ChatCompletionCreateResponse? response) =>
        {
            await Assert.That(response).IsNotNull();
            await Assert.That(response!.Choices).IsNotNull().And.IsNotEmpty();
            if (response.Choices[0].FinishReason == "stop")
            {
                containsFinishReasonStop = true;
                return;
            }

            var message = response.Choices[0].Message;
            await Assert.That(message).IsNotNull();

            if (isFirstChunk)
            {
                await Assert.That(response.Choices.Count).IsEqualTo(1);
                await Assert.That(message.Role).IsEqualTo("assistant");
                isFirstChunk = false;
            }
            else
            {
                await Assert.That(message.Content).IsNotNull();
            }

            // Accumulate independently of role framing so content is never coupled to a particular
            // chunk boundary.
            if (!string.IsNullOrEmpty(message.Content))
            {
                responseMessage.Append(message.Content);
            }
        };


        await foreach (var response in updates)
        {
            await validateResponse(response);
        }

        await Assert.That(containsFinishReasonStop).IsTrue();

        var fullResponse = responseMessage.ToString();
        Console.WriteLine(fullResponse);
        await Assert.That(fullResponse).Contains("42");

        // Take a second streamed turn over the replayed transcript. Ask the model to recall its
        // prior answer so the test measures streaming and multi-turn plumbing rather than another
        // arithmetic problem. The model-free serialization test pins the exact replayed wire shape.
        messages.Add(new ChatMessage { Role = "assistant", Content = fullResponse });
        messages.Add(new ChatMessage
        {
            Role = "user",
            Content = "What number did you give as the answer? Reply with only that number."
        });

        updates = chatClient.CompleteChatStreamingAsync(messages, CancellationToken.None).ConfigureAwait(false);
        responseMessage.Clear();
        isFirstChunk = true;
        containsFinishReasonStop = false;

        await foreach (var response in updates)
        {
            await validateResponse(response);
        }

        // Resetting the flag above makes this assertion specific to the second turn.
        await Assert.That(containsFinishReasonStop).IsTrue();

        fullResponse = responseMessage.ToString();
        Console.WriteLine(fullResponse);
        await Assert.That(fullResponse).Contains("42");
    }

    [Test]
    public async Task DirectTool_NoStreaming_Succeeds()
    {
        var chatClient = await model!.GetChatClientAsync();
        await Assert.That(chatClient).IsNotNull();

        chatClient.Settings.MaxTokens = 500;
        chatClient.Settings.Temperature = 0.0f; // for deterministic results
        chatClient.Settings.ToolChoice = OpenAIToolChoice.Required; // Force the model to make a tool call

        // Prepare messages and tools
        List<ChatMessage> messages =
        [
            new ChatMessage { Role = "system",
                              Content = "You are a helpful AI assistant. If necessary, you can use any " +
                                        "provided tools to answer the question." },
            new ChatMessage { Role = "user", Content = "What is the answer to 7 multiplied by 6?" }
        ];
        List<ToolDefinition> tools =
        [
            new ToolDefinition
            {
                Type = "function",
                Function = new FunctionDefinition()
                {
                    Name = "multiply_numbers",
                    Description = "A tool for multiplying two numbers.",
                    Parameters = new PropertyDefinition()
                    {
                        Type = "object",
                        Properties = new Dictionary<string, PropertyDefinition>()
                        {
                            {
                                "first",
                                new PropertyDefinition()
                                {
                                    Type = "integer", Description = "The first number in the operation"
                                }
                            },
                            {
                                "second",
                                new PropertyDefinition()
                                {
                                    Type = "integer",
                                    Description = "The second number in the operation"
                                }
                            }
                        },
                        Required = ["first", "second"]
                    }
                }
            }
        ];

        // Start the conversation
        var response = await chatClient.CompleteChatAsync(messages, tools).ConfigureAwait(false);
        Console.WriteLine(response.Choices[0].Message.Content);

        // Check that a tool call was generated
        await Assert.That(response).IsNotNull();
        await Assert.That(response.Choices).IsNotNull().And.IsNotEmpty();
        await Assert.That(response.Choices.Count).IsEqualTo(1);
        await Assert.That(response.Choices[0].FinishReason).IsEqualTo("tool_calls");

        await Assert.That(response.Choices[0].Message).IsNotNull();
        await Assert.That(response.Choices[0].Message.ToolCalls).IsNotNull().And.IsNotEmpty();
        var assistantToolCalls = response.Choices[0].Message.ToolCalls!;
        foreach (var assistantToolCall in assistantToolCalls)
        {
            await Assert.That(assistantToolCall.Type).IsEqualTo("function");
            await Assert.That(assistantToolCall.FunctionCall?.Name).IsEqualTo("multiply_numbers");
            await Assert.That(assistantToolCall.Id).IsNotNull();
            await Assert.That(assistantToolCall.Id).IsNotEmpty();

            var json = assistantToolCall.FunctionCall?.Arguments;
            var actual = System.Text.Json.JsonSerializer.Deserialize<Dictionary<string, int>>(json!);
            await Assert.That(actual).IsNotNull();
            await Assert.That(actual!.Keys).Contains("first");
            await Assert.That(actual.Keys).Contains("second");
            await Assert.That(actual["first"] * actual["second"]).IsEqualTo(42);
        }

        // Replay the assistant turn that issued the calls before answering them. A Chat Completions
        // payload is self-contained — every request is correlated against an empty transcript — so a
        // tool result is only matched to its call when every call travels with it. Content stays null
        // so the replay matches the content-free turn the model generated, rather than feeding its
        // tool-call marker text back.
        messages.Add(new ChatMessage { Role = "assistant", ToolCalls = assistantToolCalls });

        // Add one correlated result for every invocation and check if the model can continue correctly.
        foreach (var assistantToolCall in assistantToolCalls)
        {
            messages.Add(new ChatMessage
            {
                Role = "tool",
                ToolCallId = assistantToolCall.Id,
                Content = "7 x 6 = 42."
            });
        }

        // Prompt the model to continue the conversation after the tool call
        messages.Add(new ChatMessage { Role = "system", Content = "Respond only with the answer generated by the tool." });

        // Set tool calling back to auto so that the model can decide whether to call
        // the tool again or continue the conversation based on the new user prompt
        chatClient.Settings.ToolChoice = OpenAIToolChoice.Auto;

        // Run the next turn of the conversation
        response = await chatClient.CompleteChatAsync(messages, tools).ConfigureAwait(false);

        // Check that the conversation continued
        await Assert.That(response.Choices[0].Message.Content).IsNotNull();
        await Assert.That(response.Choices[0].Message.Content).Contains("42");
    }

    [Test]
    public async Task DirectTool_Streaming_Succeeds()
    {
        var chatClient = await model!.GetChatClientAsync();
        await Assert.That(chatClient).IsNotNull();

        chatClient.Settings.MaxTokens = 500;
        chatClient.Settings.Temperature = 0.0f; // for deterministic results
        chatClient.Settings.ToolChoice = OpenAIToolChoice.Required; // Force the model to make a tool call

        // Prepare messages and tools
        List<ChatMessage> messages =
        [
            new ChatMessage { Role = "system", Content = "You are a helpful AI assistant. If necessary, you can use any provided tools to answer the question." },
            new ChatMessage { Role = "user", Content = "What is the answer to 7 multiplied by 6?" }
        ];
        List<ToolDefinition> tools =
        [
            new ToolDefinition
            {
                Type = "function",
                Function = new FunctionDefinition()
                {
                    Name = "multiply_numbers",
                    Description = "A tool for multiplying two numbers.",
                    Parameters = new PropertyDefinition()
                    {
                        Type = "object",
                        Properties = new Dictionary<string, PropertyDefinition>()
                        {
                            { "first", new PropertyDefinition() { Type = "integer", Description = "The first number in the operation" } },
                            { "second", new PropertyDefinition() { Type = "integer", Description = "The second number in the operation" } }
                        },
                        Required = ["first", "second"]
                    }
                }
            }
        ];

        // Start the conversation
        var updates = chatClient.CompleteChatStreamingAsync(messages, tools, CancellationToken.None).ConfigureAwait(false);

        // Check that each response chunk contains the expected information
        var isFirstChunk = true;
        bool gotFinishReason = false;
        StringBuilder responseMessage = new();
        List<ChatCompletionCreateResponse> toolCallResponses = [];

        var validateResponse = async (ChatCompletionCreateResponse? response) =>
        {
            await Assert.That(response).IsNotNull();
            await Assert.That(response!.Choices).IsNotNull().And.IsNotEmpty();
            if (response.Choices[0].FinishReason == "tool_calls")
            {
                gotFinishReason = true;
                return;
            }

            await Assert.That(response.Choices.Count).IsEqualTo(1);

            var delta = response.Choices[0].Delta;
            await Assert.That(delta).IsNotNull();

            if (isFirstChunk)
            {
                await Assert.That(delta.Role).IsEqualTo("assistant");
                isFirstChunk = false;
            }
            else
            {
                if (delta.ToolCalls is { Count: > 0 })
                {
                    toolCallResponses.Add(response);
                }
            }
        };


        await foreach (var response in updates)
        {
            await validateResponse(response);
        }

        await Assert.That(gotFinishReason).IsTrue();
        await Assert.That(toolCallResponses).IsNotEmpty();
        var streamedToolCalls = toolCallResponses
            .SelectMany(response => response.Choices[0].Delta.ToolCalls!)
            .ToList();
        await Assert.That(streamedToolCalls).IsNotEmpty();

        foreach (var streamedToolCall in streamedToolCalls)
        {
            await Assert.That(streamedToolCall.Type).IsEqualTo("function");
            await Assert.That(streamedToolCall.FunctionCall?.Name).IsEqualTo("multiply_numbers");
            await Assert.That(streamedToolCall.Id).IsNotNull();
            await Assert.That(streamedToolCall.Id).IsNotEmpty();

            var json = streamedToolCall.FunctionCall?.Arguments;
            var actual = System.Text.Json.JsonSerializer.Deserialize<Dictionary<string, int>>(json!);
            await Assert.That(actual).IsNotNull();
            await Assert.That(actual!.Keys).Contains("first");
            await Assert.That(actual.Keys).Contains("second");
            await Assert.That(actual["first"] * actual["second"]).IsEqualTo(42);
        }

        // Replay the assistant turn that issued the calls before answering them — see
        // DirectTool_NoStreaming_Succeeds for why every call has to travel with its result.
        messages.Add(new ChatMessage { Role = "assistant", ToolCalls = streamedToolCalls });

        // Add one correlated result for every invocation and check if the model can continue correctly.
        foreach (var streamedToolCall in streamedToolCalls)
        {
            messages.Add(new ChatMessage
            {
                Role = "tool",
                ToolCallId = streamedToolCall.Id,
                Content = "7 x 6 = 42."
            });
        }

        // Prompt the model to continue the conversation after the tool call
        messages.Add(new ChatMessage { Role = "system", Content = "Respond only with the answer generated by the tool." });

        // Set tool calling back to auto so that the model can decide whether to call
        // the tool again or continue the conversation based on the new user prompt
        chatClient.Settings.ToolChoice = OpenAIToolChoice.Auto;

        // Run the next turn of the conversation
        updates = chatClient.CompleteChatStreamingAsync(messages, tools, CancellationToken.None).ConfigureAwait(false);
        responseMessage.Clear();

        isFirstChunk = true;
        gotFinishReason = false;
        await foreach (var response in updates)
        {
            await Assert.That(response).IsNotNull();
            await Assert.That(response.Choices).IsNotNull().And.IsNotEmpty();
            await Assert.That(response.Choices[0].Message).IsNotNull();
            if (response.Choices[0].FinishReason == "stop")
            {
                gotFinishReason = true;
                break;
            }

            var message = response.Choices[0].Message;
            if (isFirstChunk)
            {
                await Assert.That(message.Role).IsEqualTo("assistant");
                isFirstChunk = false;
            }
            else
            {
                var content = message.Content;
                await Assert.That(content).IsNotNull();
                Console.WriteLine($"Content in streaming: {content}, Finish reason: {response.Choices[0].FinishReason}");
                if (!string.IsNullOrEmpty(content))
                {
                    responseMessage.Append(content);
                }
            }
        }

        // Check that the conversation continued
        await Assert.That(gotFinishReason).IsTrue();
        var fullResponse = responseMessage.ToString();
        Console.WriteLine(fullResponse);
        await Assert.That(fullResponse).Contains("42");
    }
}
