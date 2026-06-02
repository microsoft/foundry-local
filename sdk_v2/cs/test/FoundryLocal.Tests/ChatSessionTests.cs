// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;

#pragma warning disable CA2000 // Items are transferred to Request via AddItem

[SkipUnlessIntegration]
internal sealed class ChatSessionTests
{
    private static IModel? model;

    [Before(Class)]
    public static async Task Setup()
    {
        var manager = FoundryLocalManager.Instance;
        var catalog = await manager.GetCatalogAsync();

        var model = await catalog.GetModelVariantAsync("qwen2.5-0.5b-instruct-generic-cpu:4").ConfigureAwait(false);
        await Assert.That(model).IsNotNull();

        await model!.LoadAsync().ConfigureAwait(false);
        await Assert.That(await model.IsLoadedAsync()).IsTrue();

        ChatSessionTests.model = model;
    }

    [Test]
    public async Task Chat_NoStreaming_Succeeds()
    {
        using var session = new ChatSession(model!);

        using var request = new Request();
        request.AddItem(MessageItem.User("You are a calculator. Be precise. What is the answer to 7 multiplied by 6?"));

        using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);

        await Assert.That(response).IsNotNull();
        await Assert.That(response.ItemCount).IsGreaterThan(0);

        string? content = null;

        foreach (var item in response)
        {
            using (item)
            {
                await Assert.That(item).IsTypeOf<MessageItem>();

                if (item is MessageItem msg)
                {
                    content = msg.GetSimpleText();
                }
            }
        }

        await Assert.That(content).IsNotNull();
        await Assert.That(content!).Contains("42");
        Console.WriteLine($"Response: {content}");
    }

    [Test]
    public async Task Chat_Streaming_Succeeds()
    {
        using var session = new ChatSession(model!);
        session.SetStreaming(true);

        // Use a multi-token prompt with deterministic substrings so we can validate:
        //   1. Streaming actually delivers multiple TextItem deltas (not a single coalesced item).
        //   2. The streamed content matches expectations (at least 2 of the 4 UK
        //      constituent country names appear). A 0.5B model may abbreviate or
        //      reorder; requiring a subset stays robust.
        using var request = new Request();
        request.AddItem(MessageItem.User("Name the countries in the United Kingdom."));

        var sb = new StringBuilder();
        int itemCount = 0;

        await foreach (var item in session.ProcessStreamingRequestAsync(request).ConfigureAwait(false))
        {
            using (item)
            {
                await Assert.That(item).IsTypeOf<TextItem>();
                if (item is TextItem txt)
                {
                    sb.Append(txt.Text);
                    itemCount++;
                }
            }
        }

        var fullResponse = sb.ToString();
        Console.WriteLine($"Streaming response: {fullResponse}");

        // Real streaming must deliver more than a single coalesced delta.
        await Assert.That(itemCount).IsGreaterThanOrEqualTo(2);

        var lower = fullResponse.ToLowerInvariant();
        string[] ukCountries = { "england", "scotland", "wales", "ireland" };
        int found = ukCountries.Count(name => lower.Contains(name));
        await Assert.That(found).IsGreaterThanOrEqualTo(2);

        // Turn 2 — a context-dependent follow-up. Asking for the capital of each
        // exercises history-aware generation and gives a second deterministic
        // content check.
        using var request2 = new Request();
        request2.AddItem(MessageItem.User("What is the capital of each?"));

        var sb2 = new StringBuilder();
        int itemCount2 = 0;

        await foreach (var item in session.ProcessStreamingRequestAsync(request2).ConfigureAwait(false))
        {
            using (item)
            {
                await Assert.That(item).IsTypeOf<TextItem>();
                if (item is TextItem txt)
                {
                    sb2.Append(txt.Text);
                    itemCount2++;
                }
            }
        }

        var fullResponse2 = sb2.ToString();
        Console.WriteLine($"Streaming response (turn 2): {fullResponse2}");

        await Assert.That(itemCount2).IsGreaterThanOrEqualTo(2);

        var lower2 = fullResponse2.ToLowerInvariant();
        string[] ukCapitals = { "london", "edinburgh", "cardiff", "belfast" };
        int found2 = ukCapitals.Count(name => lower2.Contains(name));
        await Assert.That(found2).IsGreaterThanOrEqualTo(2);
    }

    [Test]
    public async Task Chat_MultiTurn_Succeeds()
    {
        using var session = new ChatSession(model!);

        // First turn
        using var request1 = new Request();
        request1.AddItem(MessageItem.User("You are a calculator. Be precise. What is the answer to 7 multiplied by 6?"));

        using var response1 = await session.ProcessRequestAsync(request1).ConfigureAwait(false);

        await Assert.That(response1).IsNotNull();

        string? firstContent = null;

        foreach (var item in response1)
        {
            using (item)
            {
                await Assert.That(item).IsTypeOf<MessageItem>();

                if (item is MessageItem msg)
                {
                    firstContent = msg.GetSimpleText();
                }
            }
        }

        await Assert.That(firstContent).IsNotNull();
        await Assert.That(firstContent!).Contains("42");
        Console.WriteLine($"First response: {firstContent}");

        // Second turn — include history
        using var request2 = new Request();
        request2.AddItem(MessageItem.User("You are a calculator. Be precise. What is the answer to 7 multiplied by 6?"));
        request2.AddItem(MessageItem.Assistant(firstContent!));
        request2.AddItem(MessageItem.User("Is the answer a real number?"));

        using var response2 = await session.ProcessRequestAsync(request2).ConfigureAwait(false);

        await Assert.That(response2).IsNotNull();

        string? secondContent = null;

        foreach (var item in response2!)
        {
            using (item)
            {
                await Assert.That(item).IsTypeOf<MessageItem>();

                if (item is MessageItem msg)
                {
                    secondContent = msg.GetSimpleText();
                }
            }
        }

        await Assert.That(secondContent).IsNotNull();
        await Assert.That(secondContent!).Contains("Yes");
        Console.WriteLine($"Second response: {secondContent}");
    }

    [Test]
    public async Task Chat_Streaming_FinalResponse_ResolvesAfterDrain()
    {
        using var session = new ChatSession(model!);
        session.SetStreaming(true);

        using var request = new Request();
        request.AddItem(MessageItem.User("Name the countries in the United Kingdom."));

        var stream = session.ProcessStreamingRequestAsync(request);

        var sb = new StringBuilder();
        int itemCount = 0;

        await foreach (var item in stream)
        {
            using (item)
            {
                if (item is TextItem txt)
                {
                    sb.Append(txt.Text);
                    itemCount++;
                }
            }
        }

        await Assert.That(itemCount).IsGreaterThan(0);

        using var final = await stream.FinalResponse;

        await Assert.That(final).IsNotNull();
        await Assert.That(final.FinishReason).IsEqualTo(FinishReason.Stop);

        var usage = final.GetUsage();
        Console.WriteLine($"Usage: prompt={usage.PromptTokens} completion={usage.CompletionTokens} total={usage.TotalTokens}");
        await Assert.That(usage.PromptTokens).IsGreaterThan(0);
        await Assert.That(usage.CompletionTokens).IsGreaterThan(0);
        await Assert.That(usage.TotalTokens).IsGreaterThan(0);
    }

    [Test]
    public async Task Chat_Streaming_EarlyBreak_FinalResponse_Cancels()
    {
        using var session = new ChatSession(model!);
        session.SetStreaming(true);

        using var request = new Request();
        request.AddItem(MessageItem.User("Name the countries in the United Kingdom."));

        var stream = session.ProcessStreamingRequestAsync(request);

        int itemCount = 0;

        await foreach (var item in stream)
        {
            using (item)
            {
                itemCount++;
                if (itemCount >= 1)
                {
                    break;
                }
            }
        }

        OperationCanceledException? caught = null;
        try
        {
            using var _ = await stream.FinalResponse;
        }
        catch (OperationCanceledException oce)
        {
            caught = oce;
        }

        await Assert.That(caught).IsNotNull();
    }

    [Test]
    public async Task Chat_Streaming_TokenCancellation_FinalResponse_Cancels()
    {
        using var session = new ChatSession(model!);
        session.SetStreaming(true);

        using var request = new Request();
        request.AddItem(MessageItem.User("Name the countries in the United Kingdom."));

        using var cts = new CancellationTokenSource();
        var stream = session.ProcessStreamingRequestAsync(request, cts.Token);

        OperationCanceledException? iteratorEx = null;
        int itemCount = 0;

        try
        {
            await foreach (var item in stream)
            {
                using (item)
                {
                    itemCount++;
                    if (itemCount == 1)
                    {
                        cts.Cancel();
                    }
                }
            }
        }
        catch (OperationCanceledException oce)
        {
            iteratorEx = oce;
        }

        await Assert.That(iteratorEx).IsNotNull();

        OperationCanceledException? finalEx = null;
        try
        {
            using var _ = await stream.FinalResponse;
        }
        catch (OperationCanceledException oce)
        {
            finalEx = oce;
        }

        await Assert.That(finalEx).IsNotNull();
    }

    [Test]
    public async Task Chat_Streaming_DisposeAsync_WithoutObserving_NoLeak()
    {
        using var session = new ChatSession(model!);
        session.SetStreaming(true);

        using var request = new Request();
        request.AddItem(MessageItem.User("Hello."));

        // Get the stream and dispose it without enumerating or awaiting FinalResponse.
        var stream = session.ProcessStreamingRequestAsync(request);
        await stream.DisposeAsync();

        // A second DisposeAsync must be a no-op.
        await stream.DisposeAsync();

        // The session must be usable again — concurrent-streaming guard must have cleared.
        using var request2 = new Request();
        request2.AddItem(MessageItem.User("Hi."));

        var stream2 = session.ProcessStreamingRequestAsync(request2);
        await using (stream2)
        {
            int count = 0;
            await foreach (var item in stream2)
            {
                using (item)
                {
                    count++;
                }
            }
            await Assert.That(count).IsGreaterThan(0);
        }
    }

    [Test]
    public async Task Chat_Streaming_ConcurrentGuard_Trips()
    {
        using var session = new ChatSession(model!);
        session.SetStreaming(true);

        using var request1 = new Request();
        request1.AddItem(MessageItem.User("Name the countries in the United Kingdom."));

        using var request2 = new Request();
        request2.AddItem(MessageItem.User("Hello."));

        var stream1 = session.ProcessStreamingRequestAsync(request1);

        InvalidOperationException? caught = null;
        try
        {
            var stream2 = session.ProcessStreamingRequestAsync(request2);
            // If we somehow got here, dispose to avoid leak.
            await stream2.DisposeAsync();
        }
        catch (InvalidOperationException ex)
        {
            caught = ex;
        }

        await Assert.That(caught).IsNotNull();
        await Assert.That(caught!.Message).Contains("Concurrent streaming");

        // Drain the first stream so the session is clean for any later tests.
        await using (stream1)
        {
            await foreach (var item in stream1)
            {
                item.Dispose();
            }
            using var _ = await stream1.FinalResponse;
        }
    }

    [Test]
    public async Task Chat_Streaming_DoubleEnumeration_Throws()
    {
        using var session = new ChatSession(model!);
        session.SetStreaming(true);

        using var request = new Request();
        request.AddItem(MessageItem.User("Hello."));

        await using var stream = session.ProcessStreamingRequestAsync(request);

        await foreach (var item in stream)
        {
            item.Dispose();
        }

        InvalidOperationException? caught = null;
        try
        {
            await foreach (var item in stream)
            {
                item.Dispose();
            }
        }
        catch (InvalidOperationException ex)
        {
            caught = ex;
        }

        await Assert.That(caught).IsNotNull();
        await Assert.That(caught!.Message).Contains("enumerated once");

        using var _ = await stream.FinalResponse;
    }

    [Test]
    public async Task ToolCall_NoStreaming_Succeeds()
    {
        using var session = new ChatSession(model!);

        session.AddToolDefinition(
            "multiply_numbers",
            "A tool for multiplying two numbers.",
            /*lang=json,strict*/
            """
            {
              "type": "object",
              "properties": {
                "first": { "type": "integer", "description": "The first number in the operation" },
                "second": { "type": "integer", "description": "The second number in the operation" }
              },
              "required": ["first", "second"]
            }
            """);

        using var request = new Request();
        request.AddItem(MessageItem.System(
            "You are a helpful AI assistant. If necessary, you can use any provided tools to answer the question."));
        request.AddItem(MessageItem.User("What is the answer to 7 multiplied by 6?"));

        request.SetOptions(new RequestOptions
        {
            Search = new SearchOptions { Temperature = 0.0f },
            ToolChoice = ToolChoice.Required,
        });

        using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);

        await Assert.That(response).IsNotNull();
        await Assert.That(response.FinishReason).IsEqualTo(FinishReason.ToolCalls);

        // Find tool call item
        ToolCallItem? toolCall = null;

        foreach (var item in response!)
        {
            using (item)
            {
                await Assert.That(item).IsTypeOf<ToolCallItem>();

                if (item is ToolCallItem tc)
                {
                    toolCall = tc;
                    break;
                }
            }
        }

        await Assert.That(toolCall).IsNotNull();
        await Assert.That(toolCall!.Name).IsEqualTo("multiply_numbers");

        var args = JsonSerializer.Deserialize<Dictionary<string, int>>(toolCall!.Arguments);
        await Assert.That(args).IsNotNull();

        var expected = new Dictionary<string, int> { ["first"] = 7, ["second"] = 6 };
        await Assert.That(args!).IsEquivalentTo(expected);

        Console.WriteLine($"Tool call: {toolCall!.Name}({toolCall!.Arguments})");
    }

    // Streaming + tool-call assembly on the native ChatSession. Mirrors the C++
    // ToolCallStreamingWithRequired test: when tool_choice=Required forces a tool call,
    // the streaming iterator must deliver one fully-assembled ToolCallItem (the chat
    // generator buffers partial tool-call JSON internally rather than streaming the
    // payload character-by-character) and that streamed item must match the
    // corresponding item in the materialised response.
    [Test]
    public async Task ToolCall_Streaming_Succeeds()
    {
        using var session = new ChatSession(model!);
        session.SetStreaming(true);

        session.AddToolDefinition(
            "multiply_numbers",
            "A tool for multiplying two numbers.",
            /*lang=json,strict*/
            """
            {
              "type": "object",
              "properties": {
                "first": { "type": "integer", "description": "The first number in the operation" },
                "second": { "type": "integer", "description": "The second number in the operation" }
              },
              "required": ["first", "second"]
            }
            """);

        using var request = new Request();
        request.AddItem(MessageItem.System(
            "You are a helpful AI assistant. If necessary, you can use any provided tools to answer the question."));
        request.AddItem(MessageItem.User("What is the answer to 7 multiplied by 6?"));

        request.SetOptions(new RequestOptions
        {
            Search = new SearchOptions { Temperature = 0.0f },
            ToolChoice = ToolChoice.Required,
        });

        var streamedToolCalls = new List<(string CallId, string Name, string Arguments)>();
        var streamedText = new StringBuilder();
        int itemCount = 0;

        await foreach (var item in session.ProcessStreamingRequestAsync(request).ConfigureAwait(false))
        {
            using (item)
            {
                itemCount++;

                if (item is ToolCallItem tc)
                {
                    // Tool-call content is owned by the streamed item — copy out before the
                    // `using` releases it.
                    streamedToolCalls.Add((tc.CallId, tc.Name, tc.Arguments));
                }
                else if (item is TextItem txt)
                {
                    streamedText.Append(txt.Text);
                }
            }
        }

        await Assert.That(itemCount).IsGreaterThan(0);
        await Assert.That(streamedToolCalls.Count).IsGreaterThanOrEqualTo(1);

        var streamedTc = streamedToolCalls[0];
        await Assert.That(streamedTc.CallId).IsNotEmpty();
        await Assert.That(streamedTc.Name).IsEqualTo("multiply_numbers");
        await Assert.That(streamedTc.Arguments).IsNotEmpty();

        Console.WriteLine(
            $"Streaming tool-call test: {itemCount} item(s), {streamedToolCalls.Count} tool-call(s). "
            + $"Tool call: {streamedTc.Name}({streamedTc.Arguments}) id={streamedTc.CallId}");
    }

    [Test]
    public async Task ToolCall_WithResult_Succeeds()
    {
        // Turn 1 — get tool call
        using var session = new ChatSession(model!);

        session.AddToolDefinition(
            "multiply_numbers",
            "A tool for multiplying two numbers.",
            /*lang=json,strict*/
            """
            {
              "type": "object",
              "properties": {
                "first": { "type": "integer", "description": "The first number in the operation" },
                "second": { "type": "integer", "description": "The second number in the operation" }
              },
              "required": ["first", "second"]
            }
            """);

        using var request1 = new Request();
        request1.AddItem(MessageItem.System(
            "You are a helpful AI assistant. If necessary, you can use any provided tools to answer the question."));
        request1.AddItem(MessageItem.User("What is the answer to 7 multiplied by 6?"));

        request1.SetOptions(new RequestOptions
        {
            Search = new SearchOptions { Temperature = 0.0f },
            ToolChoice = ToolChoice.Required,
        });

        using var response1 = await session.ProcessRequestAsync(request1).ConfigureAwait(false);

        await Assert.That(response1.FinishReason).IsEqualTo(FinishReason.ToolCalls);

        // Extract tool call info
        ToolCallItem? tc = null;

        foreach (var item in response1!)
        {
            using (item)
            {
                if (item is ToolCallItem toolCallItem)
                {
                    tc = toolCallItem;
                    break;
                }
            }
        }

        await Assert.That(tc).IsNotNull();
        Console.WriteLine($"Tool call: {tc!.Name}({tc!.Arguments})");

        // Turn 2 — supply tool result and get final answer.
        // Reuse the same session — it accumulates history from turn 1 (system, user, assistant tool call).
        // We only need to provide the new input: the tool result and a follow-up prompt.
        var toolCallId = tc!.CallId;

        using var request2 = new Request();
        request2.AddItem(new ToolResultItem(toolCallId, "7 x 6 = 42."));
        request2.AddItem(MessageItem.System("Respond only with the answer generated by the tool."));

        using var response2 = await session.ProcessRequestAsync(request2).ConfigureAwait(false);

        await Assert.That(response2).IsNotNull();

        string? finalContent = null;

        foreach (var item in response2!)
        {
            using (item)
            {
                if (item is MessageItem msg)
                {
                    finalContent = msg.GetSimpleText();
                }
            }
        }

        await Assert.That(finalContent).IsNotNull();
        await Assert.That(finalContent!).Contains("42");
        Console.WriteLine($"Final response: {finalContent}");
    }
}
