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
internal sealed class SessionDisposeRaceTests
{
    private static IModel? model;

    [Before(Class)]
    public static async Task Setup()
    {
        var manager = FoundryLocalManager.Instance;
        var catalog = await manager.GetCatalogAsync();

        var loaded = await catalog.GetModelVariantAsync("qwen2.5-0.5b-instruct-generic-cpu:4").ConfigureAwait(false);

        if (loaded == null)
        {
            return;
        }

        if (!await loaded.IsCachedAsync())
        {
            return;
        }

        await loaded.LoadAsync().ConfigureAwait(false);
        model = loaded;
    }

    /// <summary>
    /// Regression test for H1: disposing a Session while a streaming enumeration is active
    /// must not crash. Dispose should signal cancellation, await the producer task, and
    /// then release the native session safely.
    /// </summary>
    [Test]
    public async Task Dispose_WhileStreaming_DoesNotCrash()
    {
        if (model == null)
        {
            throw new SkipTestException("Chat model not available");
        }

        var session = new ChatSession(model!);
        session.SetStreaming(true);

        using var request = new Request();
        request.AddItem(MessageItem.User("Write a long, detailed explanation of how a compiler works."));

        var enumerationStarted = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);

        var consumerTask = Task.Run(async () =>
        {
            try
            {
                int received = 0;
                await foreach (var item in session.ProcessStreamingRequestAsync(request).ConfigureAwait(false))
                {
                    using (item) { }

                    if (received++ == 0)
                    {
                        enumerationStarted.TrySetResult(true);
                    }
                }
            }
            catch
            {
                // Cancellation/disposal during streaming is acceptable; we only assert no crash.
                enumerationStarted.TrySetResult(true);
            }
        });

        // Wait for at least one streamed item (producer is up and running) or a brief grace period.
        var firstItemTask = enumerationStarted.Task;
        var completed = await Task.WhenAny(firstItemTask, Task.Delay(TimeSpan.FromSeconds(15))).ConfigureAwait(false);

        // Whether or not we saw an item, dispose now and verify it doesn't crash the process.
        session.Dispose();

        await consumerTask.ConfigureAwait(false);

        // If we get here, dispose-during-stream was safe (no assert needed, reaching here is the test).
    }
}
