// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;

using System.Text;
using System.Threading.Tasks;

using TUnit.Core.Exceptions;


[SkipUnlessIntegration]
internal sealed class OpenAIAudioClientTests
{
    private static IModel? model;

    [Before(Class)]
    public static async Task Setup()
    {
        var manager = FoundryLocalManager.Instance; // initialized by Utils
        var catalog = await manager.GetCatalogAsync();
        var aliasModel = await catalog.GetModelVariantAsync("openai-whisper-tiny-generic-cpu:4").ConfigureAwait(false);

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
            return;
        }

        await model.LoadAsync().ConfigureAwait(false);
        await Assert.That(await model.IsLoadedAsync()).IsTrue();

        OpenAIAudioClientTests.model = model;
    }

    [Test]
    public async Task AudioTranscription_NoStreaming_Succeeds()
    {
        if (model == null)
        {
            throw new SkipTestException("Whisper model not available");
        }

        var audioClient = await model!.GetAudioClientAsync();
        await Assert.That(audioClient).IsNotNull();

        audioClient.Settings.Language = "en";

        var audioFilePath = Utils.TestDataPath("Recording.mp3");

        var response = await audioClient.TranscribeAudioAsync(audioFilePath).ConfigureAwait(false);

        await Assert.That(response).IsNotNull();
        await Assert.That(response.Text).IsNotNull().And.IsNotEmpty();
        var content = response.Text;
        await Assert.That(content).IsEqualTo(" And lots of times you need to give people more than one link at a time. You a band could give their fans a couple new videos from the live concert behind the scenes photo gallery and album to purchase like these next few links.");
        Console.WriteLine($"Response: {content}");
    }

    [Test]
    public async Task AudioTranscription_NoStreaming_Succeeds_WithTemperature()
    {
        if (model == null)
        {
            throw new SkipTestException("Whisper model not available");
        }

        var audioClient = await model!.GetAudioClientAsync();
        await Assert.That(audioClient).IsNotNull();

        audioClient.Settings.Language = "en";
        audioClient.Settings.Temperature = 0.1f; // for deterministic results

        var audioFilePath = Utils.TestDataPath("Recording.mp3");

        var response = await audioClient.TranscribeAudioAsync(audioFilePath).ConfigureAwait(false);

        await Assert.That(response).IsNotNull();
        await Assert.That(response.Text).IsNotNull().And.IsNotEmpty();
        var content = response.Text;
        await Assert.That(content).IsEqualTo(" And lots of times you need to give people more than one link at a time. You a band could give their fans a couple new videos from the live concert behind the scenes photo gallery and album to purchase like these next few links.");
        Console.WriteLine($"Response: {content}");
    }

    [Test]
    public async Task AudioTranscription_NoStreaming_InValidFile()
    {
        if (model == null)
        {
            throw new SkipTestException("Whisper model not available");
        }

        var audioClient = await model!.GetAudioClientAsync();
        await Assert.That(audioClient).IsNotNull();

        audioClient.Settings.Language = "en";

        var audioFilePath = Utils.TestDataPath("non_exist_Recording.mp3");

        FoundryLocalException? caught = null;
        try
        {
            await audioClient.TranscribeAudioAsync(audioFilePath).ConfigureAwait(false);
        }
        catch (FoundryLocalException ex)
        {
            caught = ex;
        }

        // Assert: a FoundryLocalException must have been thrown
        await Assert.That(caught).IsNotNull();
        Console.WriteLine($"Caught exception: {caught}");
        await Assert.That(caught!.Message).Contains("Audio file not found");

    }

    [Test]
    public async Task AudioTranscription_Streaming_Succeeds()
    {
        if (model == null)
        {
            throw new SkipTestException("Whisper model not available");
        }

        var audioClient = await model!.GetAudioClientAsync();
        await Assert.That(audioClient).IsNotNull();

        audioClient.Settings.Language = "en";

        var audioFilePath = Utils.TestDataPath("Recording.mp3");

        var updates = audioClient.TranscribeAudioStreamingAsync(audioFilePath, CancellationToken.None).ConfigureAwait(false);

        StringBuilder responseMessage = new();
        await foreach (var response in updates)
        {
            await Assert.That(response).IsNotNull();
            await Assert.That(response.Text).IsNotNull().And.IsNotEmpty();
            var content = response.Text;
            responseMessage.Append(content);
        }

        var fullResponse = responseMessage.ToString();
        Console.WriteLine(fullResponse);
        await Assert.That(fullResponse).IsEqualTo(" And lots of times you need to give people more than one link at a time. You a band could give their fans a couple new videos from the live concert behind the scenes photo gallery and album to purchase like these next few links.");


    }

    [Test]
    public async Task AudioTranscription_Streaming_Succeeds_WithTemperature()
    {
        if (model == null)
        {
            throw new SkipTestException("Whisper model not available");
        }

        var audioClient = await model!.GetAudioClientAsync();
        await Assert.That(audioClient).IsNotNull();

        audioClient.Settings.Language = "en";
        audioClient.Settings.Temperature = 0.1f; // for deterministic results

        var audioFilePath = Utils.TestDataPath("Recording.mp3");

        var updates = audioClient.TranscribeAudioStreamingAsync(audioFilePath, CancellationToken.None).ConfigureAwait(false);

        StringBuilder responseMessage = new();
        await foreach (var response in updates)
        {
            await Assert.That(response).IsNotNull();
            await Assert.That(response.Text).IsNotNull().And.IsNotEmpty();
            var content = response.Text;
            responseMessage.Append(content);
        }

        var fullResponse = responseMessage.ToString();
        Console.WriteLine(fullResponse);
        await Assert.That(fullResponse).IsEqualTo(" And lots of times you need to give people more than one link at a time. You a band could give their fans a couple new videos from the live concert behind the scenes photo gallery and album to purchase like these next few links.");


    }

    [Test]
    public async Task AudioTranscription_Streaming_InvalidFiles()
    {
        if (model == null)
        {
            throw new SkipTestException("Whisper model not available");
        }

        var audioClient = await model!.GetAudioClientAsync();
        await Assert.That(audioClient).IsNotNull();

        audioClient.Settings.Language = "en";

        var audioFilePath = Utils.TestDataPath("invalid_path.mp3");

        FoundryLocalException? caught = null;
        try
        {
            await foreach (var _ in audioClient.TranscribeAudioStreamingAsync(audioFilePath, CancellationToken.None).ConfigureAwait(false))
            {
            }
        }
        catch (FoundryLocalException ex)
        {
            caught = ex;
        }

        // Assert: a FoundryLocalException must have been thrown
        await Assert.That(caught).IsNotNull();
        Console.WriteLine($"Caught exception: {caught}");
        await Assert.That(caught!.InnerException).IsNotNull();
        await Assert.That(caught.InnerException!.Message).Contains("Audio file not found");

    }
}
