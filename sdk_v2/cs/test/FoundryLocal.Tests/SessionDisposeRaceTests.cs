// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Collections;
using System.Collections.Generic;
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

    [Test]
    public async Task Dispose_DuringSetOptionsMarshaling_PreventsNativeCall()
    {
        if (model == null)
        {
            throw new SkipTestException("Chat model not available");
        }

        using var additionalOptions = new BlockingDictionary();
        using var session = new ChatSession(model!);
        var options = new RequestOptions { AdditionalOptions = additionalOptions };
        var setOptionsTask = Task.Run(() => session.SetOptions(options));
        Task? disposeTask = null;

        try
        {
            var enumerationStarted = await Task.WhenAny(
                additionalOptions.EnumerationStarted,
                Task.Delay(TimeSpan.FromSeconds(5))).ConfigureAwait(false);
            if (!ReferenceEquals(enumerationStarted, additionalOptions.EnumerationStarted))
            {
                throw new System.TimeoutException("SetOptions did not begin enumerating options within five seconds.");
            }

            disposeTask = Task.Run(session.Dispose);
            var disposeCompleted = await Task.WhenAny(
                disposeTask,
                Task.Delay(TimeSpan.FromSeconds(5))).ConfigureAwait(false);
            if (!ReferenceEquals(disposeCompleted, disposeTask))
            {
                throw new System.TimeoutException("Dispose did not complete during managed option marshaling.");
            }

            await disposeTask.ConfigureAwait(false);
        }
        finally
        {
            additionalOptions.ReleaseEnumeration();

            if (disposeTask != null && !disposeTask.IsCompleted)
            {
                await disposeTask.ConfigureAwait(false);
            }
        }

        await Assert.That(async () => await setOptionsTask.ConfigureAwait(false))
            .Throws<ObjectDisposedException>();
        await Assert.That(() => session.SetOptions(new RequestOptions())).Throws<ObjectDisposedException>();
        await Assert.That(() => session.SetStreaming(true)).Throws<ObjectDisposedException>();
        await Assert.That(session.Cancel).Throws<ObjectDisposedException>();
        await Assert.That(() => session.AddToolDefinition("tool", "description", "{}"))
            .Throws<ObjectDisposedException>();
        await Assert.That(() => session.RemoveToolDefinition("tool")).Throws<ObjectDisposedException>();
        await Assert.That(() =>
        {
            _ = session.TurnCount;
        }).Throws<ObjectDisposedException>();
        await Assert.That(() => session.UndoTurns(1)).Throws<ObjectDisposedException>();
    }

    private sealed class BlockingDictionary : IDictionary<string, string>, IDisposable
    {
        private readonly ManualResetEventSlim _release = new(false);
        private readonly TaskCompletionSource<bool> _enumerationStarted =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        internal Task EnumerationStarted => _enumerationStarted.Task;

        public string this[string key]
        {
            get => throw new KeyNotFoundException();
            set => throw new NotSupportedException();
        }

        public ICollection<string> Keys => ["language"];

        public ICollection<string> Values => ["en"];

        public int Count => 1;

        public bool IsReadOnly => true;

        public void Add(string key, string value) => throw new NotSupportedException();

        public void Add(KeyValuePair<string, string> item) => throw new NotSupportedException();

        public void Clear() => throw new NotSupportedException();

        public bool Contains(KeyValuePair<string, string> item) =>
            item.Key == "language" && item.Value == "en";

        public bool ContainsKey(string key) => key == "language";

        public void CopyTo(KeyValuePair<string, string>[] array, int arrayIndex) =>
            array[arrayIndex] = new KeyValuePair<string, string>("language", "en");

        public IEnumerator<KeyValuePair<string, string>> GetEnumerator()
        {
            _enumerationStarted.TrySetResult(true);
            _release.Wait();
            yield return new KeyValuePair<string, string>("language", "en");
        }

        public bool Remove(string key) => throw new NotSupportedException();

        public bool Remove(KeyValuePair<string, string> item) => throw new NotSupportedException();

        public bool TryGetValue(string key, out string value)
        {
            value = "en";
            return key == "language";
        }

        IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

        public void Dispose() => _release.Dispose();

        internal void ReleaseEnumeration() => _release.Set();
    }
}
