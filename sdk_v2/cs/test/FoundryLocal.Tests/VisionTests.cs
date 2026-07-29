// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Text;
using System.Threading.Tasks;

using TUnit.Core.Exceptions;

#pragma warning disable CA2000 // Items are transferred to Request via AddItem

[SkipUnlessIntegration]
internal sealed class VisionTests
{
    private static IModel? model;

    private static string TestImagePath => Utils.TestDataPath("Taittinger.jpg");

    [Before(Class)]
    public static async Task Setup()
    {
        try
        {
            var manager = FoundryLocalManager.Instance;
            var catalog = await manager.GetCatalogAsync();

            var allModels = await catalog.ListModelsAsync().ConfigureAwait(false);

            // Find a model with the vision-language-chat task.
            IModel? visionModel = null;

            foreach (var m in allModels)
            {

                if (m.Info.Task != "vision-language-chat")
                {
                    continue;
                }

                // Prefer a CPU variant so we don't depend on GPU/NPU EP bootstrapping.
                foreach (var v in m.Variants)
                {
                    if (v.Info.Runtime?.DeviceType == DeviceType.CPU && v.Info.Cached)
                    {
                        visionModel = v;
                        break;
                    }
                }
            }

            if (visionModel == null)
            {
                Console.WriteLine("VisionTests: no vision-language-chat model found in catalog — skipping all tests");
                return;
            }

            if (!await visionModel.IsCachedAsync().ConfigureAwait(false))
            {
                Console.WriteLine($"VisionTests: vision model '{visionModel.Id}' is not cached — skipping all tests");
                return;
            }

            await visionModel.LoadAsync().ConfigureAwait(false);
            await Assert.That(await visionModel.IsLoadedAsync()).IsTrue();

            model = visionModel;
            Console.WriteLine($"VisionTests: using vision model '{model.Id}'");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"VisionTests setup failed: {ex}");
            throw;
        }
    }

    [Test]
    public async Task Vision_ImageBytesInput_ProducesResponse()
    {
        if (model == null)
        {
            throw new SkipTestException("Vision model not available");
        }

        // Use the sync overload so the same source compiles on net462, which does not have
        // File.ReadAllBytesAsync. The test image is a few KB — blocking briefly is fine.
        var imageBytes = File.ReadAllBytes(TestImagePath);

        using var session = new ChatSession(model!);
        session.SetOptions(new RequestOptions
        {
            Search = new SearchOptions { MaxOutputTokens = 512, Temperature = 0f },
        });

        using var request = new Request();

        var parts = new Item[]
        {
            new TextItem("Describe this image in one short sentence."),
            new ImageItem("jpeg", new ReadOnlyMemory<byte>(imageBytes)),
        };

        request.AddItem(new MessageItem(MessageRole.User, parts));

        using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);

        await Assert.That(response).IsNotNull();
        await Assert.That(response.ItemCount).IsGreaterThan(0);

        string? content = null;

        foreach (var item in response)
        {
            using (item)
            {
                if (item is MessageItem msg)
                {
                    content = msg.GetSimpleText();
                }
            }
        }

        await Assert.That(content).IsNotNull().And.IsNotEmpty();
        // IndexOf with StringComparison is available on netstandard2.0; the
        // string.Contains(string, StringComparison) overload is net8+ only.
#pragma warning disable CA2249 // Use string.Contains — intentional for net462 source compat
        await Assert.That(content!.IndexOf("bottle", StringComparison.OrdinalIgnoreCase) >= 0).IsTrue();
#pragma warning restore CA2249
        Console.WriteLine($"Vision response: {content}");
    }

    [Test]
    public async Task Vision_ImageBytesInput_Streaming_ProducesTokens()
    {
        if (model == null)
        {
            throw new SkipTestException("Vision model not available");
        }

        // Use the sync overload so the same source compiles on net462, which does not have
        // File.ReadAllBytesAsync. The test image is a few KB — blocking briefly is fine.
        var imageBytes = File.ReadAllBytes(TestImagePath);

        using var session = new ChatSession(model!);
        session.SetOptions(new RequestOptions
        {
            Search = new SearchOptions { MaxOutputTokens = 512, Temperature = 0f },
        });
        session.SetStreaming(true);

        using var request = new Request();

        var parts = new Item[]
        {
            new TextItem("Describe this image in one short sentence."),
            new ImageItem("jpeg", new ReadOnlyMemory<byte>(imageBytes)),
        };

        request.AddItem(new MessageItem(MessageRole.User, parts));

        var sb = new StringBuilder();

        await foreach (var item in session.ProcessStreamingRequestAsync(request).ConfigureAwait(false))
        {
            using (item)
            {
                if (item is TextItem txt)
                {
                    sb.Append(txt.Text);
                }
            }
        }

        var fullResponse = sb.ToString();
        Console.WriteLine($"Vision streaming response: {fullResponse}");
        await Assert.That(fullResponse).IsNotEmpty();
#pragma warning disable CA2249 // Use string.Contains — intentional for net462 source compat
        await Assert.That(fullResponse.IndexOf("bottle", StringComparison.OrdinalIgnoreCase) >= 0).IsTrue();
#pragma warning restore CA2249
    }
}
