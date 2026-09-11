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
        await Assert.That(response.Choices[0].Message.ToolCalls?.Count).IsEqualTo(1);
        await Assert.That(response.Choices[0].Message.ToolCalls?[0].Type).IsEqualTo("function");
        await Assert.That(response.Choices[0].Message.ToolCalls?[0].FunctionCall?.Name).IsEqualTo("multiply_numbers");

        var expected = new Dictionary<string, int>
        {
            ["first"] = 7,
            ["second"] = 6
        };

        var json = response.Choices[0].Message.ToolCalls?[0].FunctionCall?.Arguments;
        var actual = System.Text.Json.JsonSerializer.Deserialize<Dictionary<string, int>>(json!);
        await Assert.That(actual).IsEquivalentTo(expected);

        // Replay the assistant turn that issued the call before answering it. A Chat Completions
        // payload is self-contained — every request is correlated against an empty transcript — so a
        // tool result is only matched to its call when that call travels with it. Content stays null
        // so the replay matches the content-free turn the model generated, rather than feeding its
        // tool-call marker text back.
        var assistantToolCall = response.Choices[0].Message.ToolCalls![0];
        await Assert.That(assistantToolCall.Id).IsNotNull();
        await Assert.That(assistantToolCall.Id).IsNotEmpty();

        messages.Add(new ChatMessage { Role = "assistant", ToolCalls = [assistantToolCall] });

        // Add the response from invoking the tool call to the conversation and check if the model can continue correctly
        var toolCallResponse = "7 x 6 = 42.";
        messages.Add(new ChatMessage
        {
            Role = "tool",
            ToolCallId = assistantToolCall.Id,
            Content = toolCallResponse
        });

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
        ChatCompletionCreateResponse? toolCallResponse = null;

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
                // we only expect one callback with all the args so we're not accumulating across multiple delta chunks
                await Assert.That(toolCallResponse).IsNull();
                toolCallResponse = response;
            }
        };


        await foreach (var response in updates)
        {
            await validateResponse(response);
        }

        await Assert.That(gotFinishReason).IsTrue();
        await Assert.That(toolCallResponse).IsNotNull();
        await Assert.That(toolCallResponse!.Choices.Count).IsEqualTo(1);
        await Assert.That(toolCallResponse.Choices[0].Delta.ToolCalls).IsNotNull();
        await Assert.That(toolCallResponse.Choices[0].Delta.ToolCalls?.Count).IsEqualTo(1);
        await Assert.That(toolCallResponse.Choices[0].Delta.ToolCalls?[0].Type).IsEqualTo("function");
        await Assert.That(toolCallResponse.Choices[0].Delta.ToolCalls?[0].FunctionCall?.Name).IsEqualTo("multiply_numbers");

        var expected = new Dictionary<string, int>
        {
            ["first"] = 7,
            ["second"] = 6
        };

        var json = toolCallResponse.Choices[0].Message.ToolCalls?[0].FunctionCall?.Arguments;
        var actual = System.Text.Json.JsonSerializer.Deserialize<Dictionary<string, int>>(json!);
        await Assert.That(actual).IsEquivalentTo(expected);

        // Replay the assistant turn that issued the call before answering it — see
        // DirectTool_NoStreaming_Succeeds for why the call has to travel with its result.
        var streamedToolCall = toolCallResponse.Choices[0].Delta.ToolCalls![0];
        await Assert.That(streamedToolCall.Id).IsNotNull();
        await Assert.That(streamedToolCall.Id).IsNotEmpty();

        messages.Add(new ChatMessage { Role = "assistant", ToolCalls = [streamedToolCall] });

        // Add the response from invoking the tool call to the conversation and check if the model can continue correctly
        var toolResponse = "7 x 6 = 42.";
        messages.Add(new ChatMessage
        {
            Role = "tool",
            ToolCallId = streamedToolCall.Id,
            Content = toolResponse
        });

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
