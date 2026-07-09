// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Threading.Tasks;

using TUnit.Core.Exceptions;

#pragma warning disable CA2000 // Items are transferred to Request via AddItem

[SkipUnlessIntegration]
internal sealed class SessionItemLifecycleTests
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

        SessionItemLifecycleTests.model = model;
    }

    [Test]
    public async Task Request_AddItem_IncrementsCount()
    {
        using var request = new Request();

        await Assert.That(request.ItemCount).IsEqualTo(0);

        request.AddItem(MessageItem.User("first"));
        await Assert.That(request.ItemCount).IsEqualTo(1);

        request.AddItem(MessageItem.User("second"));
        await Assert.That(request.ItemCount).IsEqualTo(2);

        request.AddItem(MessageItem.System("third"));
        await Assert.That(request.ItemCount).IsEqualTo(3);
    }

    [Test]
    public async Task Request_GetItem_ReturnsCorrectType()
    {
        using var request = new Request();
        request.AddItem(MessageItem.User("test message"));

        using var item = request.GetItem(0);

        await Assert.That(item).IsTypeOf<MessageItem>();

        var msg = (MessageItem)item;
        await Assert.That(msg.Role).IsEqualTo(MessageRole.User);
    }

    [Test]
    public async Task MessageItem_System_User_Assistant_Developer_RoundTrip()
    {
        {
            using var msg = MessageItem.System("sys content");
            await Assert.That(msg.Role).IsEqualTo(MessageRole.System);
            await Assert.That(msg.GetSimpleText()).IsEqualTo("sys content");
        }

        {
            using var msg = MessageItem.User("usr content");
            await Assert.That(msg.Role).IsEqualTo(MessageRole.User);
            await Assert.That(msg.GetSimpleText()).IsEqualTo("usr content");
        }

        {
            using var msg = MessageItem.Assistant("asst content");
            await Assert.That(msg.Role).IsEqualTo(MessageRole.Assistant);
            await Assert.That(msg.GetSimpleText()).IsEqualTo("asst content");
        }

        {
            using var msg = MessageItem.Developer("dev content");
            await Assert.That(msg.Role).IsEqualTo(MessageRole.Developer);
            await Assert.That(msg.GetSimpleText()).IsEqualTo("dev content");
        }
    }

    [Test]
    public async Task MessageItem_MultiPart_Constructor()
    {
        var parts = new Item[]
        {
            new TextItem("part one"),
            new TextItem("part two"),
        };

        using var msg = new MessageItem(MessageRole.User, parts);

        await Assert.That(msg.Role).IsEqualTo(MessageRole.User);
        await Assert.That(msg.Parts.Count).IsEqualTo(2);
        await Assert.That(msg.IsSimpleText()).IsFalse();
    }

    [Test]
    public async Task TextItem_DefaultAndReasoning_Types()
    {
        {
            using var item = new TextItem("hello");
            await Assert.That(item.Text).IsEqualTo("hello");
            await Assert.That(item.Type).IsEqualTo(TextItemType.Default);
        }

        {
            using var item = new TextItem("think", TextItemType.Reasoning);
            await Assert.That(item.Text).IsEqualTo("think");
            await Assert.That(item.Type).IsEqualTo(TextItemType.Reasoning);
        }
    }

    [Test]
    public async Task Response_Enumeration_MatchesItemCount()
    {
        if (model == null)
        {
            throw new SkipTestException("Chat model not available");
        }

        using var session = new ChatSession(model!);

        using var request = new Request();
        request.AddItem(MessageItem.User("Say hello."));

        using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);

        await Assert.That(response).IsNotNull();
        await Assert.That(response.ItemCount).IsGreaterThan(0);

        int enumeratedCount = 0;

        foreach (var item in response)
        {
            using (item)
            {
                enumeratedCount++;
            }
        }

        await Assert.That(enumeratedCount).IsEqualTo(response.ItemCount);
        Console.WriteLine($"Response had {enumeratedCount} item(s)");
    }

    [Test]
    public async Task ChatSession_TurnCount_IncreasesAfterRequest()
    {
        if (model == null)
        {
            throw new SkipTestException("Chat model not available");
        }

        using var session = new ChatSession(model!);

        await Assert.That(session.TurnCount).IsEqualTo((ulong)0);

        using var request = new Request();
        request.AddItem(MessageItem.User("Say hello."));

        using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);
        await Assert.That(response).IsNotNull();

        await Assert.That(session.TurnCount).IsEqualTo((ulong)1);
        Console.WriteLine($"TurnCount after one request: {session.TurnCount}");
    }

    [Test]
    public async Task ChatSession_UndoTurns_DecrementsTurnCount()
    {
        if (model == null)
        {
            throw new SkipTestException("Chat model not available");
        }

        using var session = new ChatSession(model!);

        // Turn 1
        {
            using var request = new Request();
            request.AddItem(MessageItem.User("Say hello."));
            using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);
        }

        // Turn 2
        {
            using var request = new Request();
            request.AddItem(MessageItem.User("Say goodbye."));
            using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);
        }

        await Assert.That(session.TurnCount).IsEqualTo((ulong)2);

        session.UndoTurns(1);

        await Assert.That(session.TurnCount).IsEqualTo((ulong)1);
        Console.WriteLine($"TurnCount after undo: {session.TurnCount}");
    }

    [Test]
    public async Task ChatSession_SetOptions_AppliedToRequest()
    {
        if (model == null)
        {
            throw new SkipTestException("Chat model not available");
        }

        using var session = new ChatSession(model!);
        session.SetOptions(new RequestOptions { Search = new SearchOptions { MaxOutputTokens = 3 } });

        using var request = new Request();
        request.AddItem(MessageItem.User(
            "Write an essay about the history of computing with a minimum of 50 words."));

        using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);

        await Assert.That(response).IsNotNull();
        await Assert.That(response.FinishReason).IsEqualTo(FinishReason.Length);
        Console.WriteLine($"FinishReason with max_output_tokens=3: {response.FinishReason}");
    }

    [Test]
    public async Task Response_GetUsage_ReturnsNonZeroTokens()
    {
        if (model == null)
        {
            throw new SkipTestException("Chat model not available");
        }

        using var session = new ChatSession(model!);

        using var request = new Request();
        request.AddItem(MessageItem.User("What is 2+2?"));

        using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);

        await Assert.That(response).IsNotNull();

        var usage = response.GetUsage();

        await Assert.That(usage.PromptTokens).IsGreaterThan(0);
        await Assert.That(usage.CompletionTokens).IsGreaterThan(0);
        await Assert.That(usage.TotalTokens).IsGreaterThan(0);
        await Assert.That(usage.TotalTokens).IsEqualTo(usage.PromptTokens + usage.CompletionTokens);

        Console.WriteLine(
            $"Usage — prompt: {usage.PromptTokens}, completion: {usage.CompletionTokens}, total: {usage.TotalTokens}");
    }
}
