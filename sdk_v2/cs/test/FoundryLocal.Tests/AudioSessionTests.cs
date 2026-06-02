// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Collections.Generic;
using System.Text;
using System.Threading.Tasks;

using TUnit.Core.Exceptions;

#pragma warning disable CA2000 // Items are transferred to Request via AddItem

[SkipUnlessIntegration]
internal sealed class AudioSessionTests
{
    private static IModel? model;

    private const string ExpectedTranscription =
        " And lots of times you need to give people more than one link at a time." +
        " You a band could give their fans a couple new videos from the live concert" +
        " behind the scenes photo gallery and album to purchase like these next few links.";

    [Before(Class)]
    public static async Task Setup()
    {
        try
        {
            var manager = FoundryLocalManager.Instance;
            var catalog = await manager.GetCatalogAsync();

            var aliasModel = await catalog.GetModelAsync("whisper-tiny").ConfigureAwait(false);

            if (aliasModel == null)
            {
                return;
            }

            // Pick the CPU variant — CUDA/DML variants require an EP bootstrapper that may not be registered.
            var model = aliasModel.Variants.FirstOrDefault(v => v.Info.Runtime?.DeviceType == DeviceType.CPU);

            if (model == null)
            {
                return;
            }

            if (!await model.IsCachedAsync())
            {
                await model.DownloadAsync().ConfigureAwait(false);
                // return;
            }

            await model.LoadAsync().ConfigureAwait(false);
            await Assert.That(await model.IsLoadedAsync()).IsTrue();

            AudioSessionTests.model = model;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"Setup failed: {ex}");
            throw;
        }
    }

    [Test]
    public async Task Transcribe_NoStreaming_Succeeds()
    {
        if (model == null)
        {
            throw new SkipTestException("Whisper model not available");
        }

        using var session = new AudioSession(model!);
        session.SetOptions(new RequestOptions
        {
            AdditionalOptions = new Dictionary<string, string> { ["language"] = "en" },
        });

        var audioFilePath = Utils.TestDataPath("Recording.mp3");

        using var request = new Request();
        request.AddItem(new AudioItem(audioFilePath));

        using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);

        await Assert.That(response).IsNotNull();

        string? text = null;

        for (int i = 0; i < response.ItemCount; i++)
        {
            using var item = response.GetItem(i);

            if (item is TextItem txt)
            {
                text = txt.Text;
                break;
            }
        }

        await Assert.That(text).IsNotNull().And.IsNotEmpty();
        await Assert.That(text!).IsEqualTo(ExpectedTranscription);
        Console.WriteLine($"Response: {text}");
    }

    [Test]
    public async Task Transcribe_Streaming_Succeeds()
    {
        if (model == null)
        {
            throw new SkipTestException("Whisper model not available");
        }

        using var session = new AudioSession(model!);
        session.SetOptions(new RequestOptions
        {
            AdditionalOptions = new Dictionary<string, string> { ["language"] = "en" },
        });
        session.SetStreaming(true);

        var audioFilePath = Utils.TestDataPath("Recording.mp3");

        using var request = new Request();
        request.AddItem(new AudioItem(audioFilePath));

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
        Console.WriteLine($"Streaming response: {fullResponse}");
        await Assert.That(fullResponse).IsEqualTo(ExpectedTranscription);
    }

    [Test]
    public async Task Transcribe_Streaming_FinalResponse_AggregatesTranscript()
    {
        if (model == null)
        {
            throw new SkipTestException("Whisper model not available");
        }

        using var session = new AudioSession(model!);
        session.SetOptions(new RequestOptions
        {
            AdditionalOptions = new Dictionary<string, string> { ["language"] = "en" },
        });
        session.SetStreaming(true);

        var audioFilePath = Utils.TestDataPath("Recording.mp3");

        using var request = new Request();
        request.AddItem(new AudioItem(audioFilePath));

        var stream = session.ProcessStreamingRequestAsync(request);

        var sb = new StringBuilder();
        int streamedCount = 0;

        await foreach (var item in stream)
        {
            using (item)
            {
                if (item is TextItem txt)
                {
                    sb.Append(txt.Text);
                    streamedCount++;
                }
            }
        }

        await Assert.That(streamedCount).IsGreaterThan(0);
        await Assert.That(sb.ToString()).IsEqualTo(ExpectedTranscription);

        using var final = await stream.FinalResponse;

        await Assert.That(final).IsNotNull();
        await Assert.That(final.ItemCount).IsGreaterThan(0);

        string? aggregated = null;
        for (int i = 0; i < final.ItemCount; i++)
        {
            var item = final.GetItem(i);
            if (item is TextItem txt)
            {
                aggregated = txt.Text;
                break;
            }
        }

        await Assert.That(aggregated).IsNotNull();
        await Assert.That(aggregated!).IsEqualTo(ExpectedTranscription);
    }

    [Test]
    public async Task Transcribe_InvalidFile_Throws()
    {
        if (model == null)
        {
            throw new SkipTestException("Whisper model not available");
        }

        using var session = new AudioSession(model!);

        var audioFilePath = Utils.TestDataPath("non_exist_Recording.mp3");

        using var request = new Request();
        request.AddItem(new AudioItem(audioFilePath));

        FoundryLocalException? caught = null;

        try
        {
            using var response = await session.ProcessRequestAsync(request).ConfigureAwait(false);
        }
        catch (FoundryLocalException ex)
        {
            caught = ex;
        }

        await Assert.That(caught).IsNotNull();
        Console.WriteLine($"Caught exception: {caught}");
    }
}
