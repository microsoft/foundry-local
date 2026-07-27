// --------------------------------------------------------------------------------------------------------------------
// <copyright company="Microsoft">
//   Copyright (c) Microsoft. All rights reserved.
// </copyright>
// --------------------------------------------------------------------------------------------------------------------

namespace Microsoft.AI.Foundry.Local.Tests;
using Microsoft.AI.Foundry.Local.OpenAI;

using TUnit.Core.Exceptions;

internal sealed class LiveAudioTranscriptionTests
{
    // --- LiveAudioTranscriptionResponse.FromJson tests ---

    [Test]
    public async Task FromJson_ParsesTextAndIsFinal()
    {
        var json = """{"is_final":true,"text":"hello world","start_time":null,"end_time":null}""";

        var result = LiveAudioTranscriptionResponse.FromJson(json);

        await Assert.That(result.Content).IsNotNull();
        await Assert.That(result.Content!.Count).IsEqualTo(1);
        await Assert.That(result.Content[0].Text).IsEqualTo("hello world");
        await Assert.That(result.Content[0].Transcript).IsEqualTo("hello world");
        await Assert.That(result.IsFinal).IsTrue();
    }

    [Test]
    public async Task FromJson_MapsTimingFields()
    {
        var json = """{"is_final":false,"text":"partial","start_time":1.5,"end_time":3.0}""";

        var result = LiveAudioTranscriptionResponse.FromJson(json);

        await Assert.That(result.Content?[0]?.Text).IsEqualTo("partial");
        await Assert.That(result.IsFinal).IsFalse();
        await Assert.That(result.StartTime).IsEqualTo(1.5);
        await Assert.That(result.EndTime).IsEqualTo(3.0);
    }

    [Test]
    public async Task FromJson_EmptyText_ParsesSuccessfully()
    {
        var json = """{"is_final":true,"text":"","start_time":null,"end_time":null}""";

        var result = LiveAudioTranscriptionResponse.FromJson(json);

        await Assert.That(result.Content?[0]?.Text).IsEqualTo("");
        await Assert.That(result.IsFinal).IsTrue();
    }

    [Test]
    public async Task FromJson_OnlyStartTime_SetsStartTime()
    {
        var json = """{"is_final":true,"text":"word","start_time":2.0,"end_time":null}""";

        var result = LiveAudioTranscriptionResponse.FromJson(json);

        await Assert.That(result.StartTime).IsEqualTo(2.0);
        await Assert.That(result.EndTime).IsNull();
        await Assert.That(result.Content?[0]?.Text).IsEqualTo("word");
    }

    [Test]
    public async Task FromJson_InvalidJson_Throws()
    {
        var ex = Assert.Throws<Exception>(() =>
            LiveAudioTranscriptionResponse.FromJson("not valid json"));
        await Assert.That(ex).IsNotNull();
    }

    [Test]
    public async Task FromJson_ContentHasTextAndTranscript()
    {
        var json = """{"is_final":true,"text":"test","start_time":null,"end_time":null}""";

        var result = LiveAudioTranscriptionResponse.FromJson(json);

        // Both Text and Transcript should have the same value
        await Assert.That(result.Content?[0]?.Text).IsEqualTo("test");
        await Assert.That(result.Content?[0]?.Transcript).IsEqualTo("test");
    }

    [Test]
    public async Task FromJson_ParsesIdField()
    {
        var json = """{"id":"audio_xyz","text":"hi","is_final":true,"start_time":0.0,"end_time":1.0}""";

        var result = LiveAudioTranscriptionResponse.FromJson(json);

        await Assert.That(result.Id).IsEqualTo("audio_xyz");
    }

    [Test]
    public async Task FromJson_MissingId_DefaultsToEmpty()
    {
        var json = """{"is_final":true,"text":"hi","start_time":null,"end_time":null}""";

        var result = LiveAudioTranscriptionResponse.FromJson(json);

        await Assert.That(result.Id).IsEqualTo(string.Empty);
    }

    // --- LiveAudioTranscriptionOptions tests ---

    [Test]
    public async Task Options_DefaultValues()
    {
        var options = new LiveAudioTranscriptionSession.LiveAudioTranscriptionOptions();

        await Assert.That(options.SampleRate).IsEqualTo(16000);
        await Assert.That(options.Channels).IsEqualTo(1);
        await Assert.That(options.Language).IsNull();
        await Assert.That(options.PushQueueCapacity).IsEqualTo(100);
    }

    // --- CoreErrorResponse tests ---

    [Test]
    public async Task CoreErrorResponse_TryParse_ValidJson()
    {
        var json = """{"code":"ASR_SESSION_NOT_FOUND","message":"Session not found","isTransient":false}""";

        var error = CoreErrorResponse.TryParse(json);

        await Assert.That(error).IsNotNull();
        await Assert.That(error!.Code).IsEqualTo("ASR_SESSION_NOT_FOUND");
        await Assert.That(error.Message).IsEqualTo("Session not found");
        await Assert.That(error.IsTransient).IsFalse();
    }

    [Test]
    public async Task CoreErrorResponse_TryParse_InvalidJson_ReturnsNull()
    {
        var result = CoreErrorResponse.TryParse("not json");
        await Assert.That(result).IsNull();
    }

    [Test]
    public async Task CoreErrorResponse_TryParse_TransientError()
    {
        var json = """{"code":"BUSY","message":"Model busy","isTransient":true}""";

        var error = CoreErrorResponse.TryParse(json);

        await Assert.That(error).IsNotNull();
        await Assert.That(error!.IsTransient).IsTrue();
    }

    // --- Session state guard tests ---

    [Test]
    public async Task AppendAsync_BeforeStart_Throws()
    {
        await using var session = new LiveAudioTranscriptionSession("test-model", null!);
        var data = new ReadOnlyMemory<byte>(new byte[100]);

        FoundryLocalException? caught = null;
        try
        {
            await session.AppendAsync(data);
        }
        catch (FoundryLocalException ex)
        {
            caught = ex;
        }

        await Assert.That(caught).IsNotNull();
    }

    [Test]
    public async Task GetStream_BeforeStart_Throws()
    {
        await using var session = new LiveAudioTranscriptionSession("test-model", null!);

        FoundryLocalException? caught = null;
        try
        {
            await foreach (var _ in session.GetStream())
            {
                // should not reach here
            }
        }
        catch (FoundryLocalException ex)
        {
            caught = ex;
        }

        await Assert.That(caught).IsNotNull();
    }

    // --- E2E streaming test with synthetic PCM audio ---

    [Test]
    [SkipUnlessIntegration]
    public async Task LiveStreaming_E2E_WithSyntheticPCM_ReturnsValidResponse()
    {
        if (streamingAudioModel == null || !await streamingAudioModel.IsLoadedAsync())
        {
            throw new SkipTestException("nemotron streaming audio model not available");
        }

        var audioClient = await streamingAudioModel.GetAudioClientAsync();
        await using var session = audioClient.CreateLiveTranscriptionSession();
        session.Settings.SampleRate = 16000;
        session.Settings.Channels = 1;
        session.Settings.BitsPerSample = 16;

        await session.StartAsync();

        // Start collecting results in background (must start before pushing audio)
        var results = new List<LiveAudioTranscriptionResponse>();
        var readTask = Task.Run(async () =>
        {
            await foreach (var result in session.GetStream())
            {
                results.Add(result);
            }
        });

        // Generate ~2 seconds of synthetic PCM audio (440Hz sine wave, 16kHz, 16-bit mono)
        const int sampleRate = 16000;
        const int durationSeconds = 2;
        const double frequency = 440.0;
        int totalSamples = sampleRate * durationSeconds;
        var pcmBytes = new byte[totalSamples * 2]; // 16-bit = 2 bytes per sample

        for (int i = 0; i < totalSamples; i++)
        {
            double t = (double)i / sampleRate;
            short sample = (short)(short.MaxValue * 0.5 * Math.Sin(2 * Math.PI * frequency * t));
            pcmBytes[i * 2] = (byte)(sample & 0xFF);
            pcmBytes[i * 2 + 1] = (byte)((sample >> 8) & 0xFF);
        }

        // Push audio in chunks (100ms each, matching typical mic callback size)
        int chunkSize = sampleRate / 10 * 2; // 100ms of 16-bit audio
        for (int offset = 0; offset < pcmBytes.Length; offset += chunkSize)
        {
            int len = Math.Min(chunkSize, pcmBytes.Length - offset);
            await session.AppendAsync(new ReadOnlyMemory<byte>(pcmBytes, offset, len));
        }

        // Stop session to flush remaining audio and complete the stream
        await session.StopAsync();
        await readTask;

        // Verify response attributes — synthetic audio may or may not produce text,
        // but the response objects should be properly structured
        foreach (var result in results)
        {
            // Verify ConversationItem-shaped response
            await Assert.That(result.Content).IsNotNull();
            await Assert.That(result.Content!.Count).IsGreaterThan(0);
            await Assert.That(result.Content[0].Text).IsNotNull();
            // Text and Transcript should be the same
            await Assert.That(result.Content[0].Transcript).IsEqualTo(result.Content[0].Text);
        }
    }

    // --- Streaming tests using Recording.pcm (matching C++ streaming_audio_test.cc) ---

    private static IModel? streamingAudioModel;

    [Before(Class)]
    public static async Task SetupStreamingAudioModel()
    {
        // The class contains both pure unit tests and integration tests. When the manager
        // failed to initialize, the integration tests skip via [SkipUnlessIntegration] but
        // this Before(Class) hook still runs — bail out instead of throwing on Instance.
        if (!Utils.IntegrationTestsAvailable)
        {
            return;
        }

        var manager = FoundryLocalManager.Instance;
        var catalog = await manager.GetCatalogAsync();
        streamingAudioModel = await catalog.GetModelAsync("nemotron-speech-streaming-en-0.6b");

        if (streamingAudioModel != null && await streamingAudioModel.IsCachedAsync())
        {
            await streamingAudioModel.LoadAsync();
        }
    }

    private static byte[] LoadRecordingPcm()
    {
        var pcmPath = Utils.TestDataPath("Recording.pcm");

        if (!File.Exists(pcmPath))
        {
            throw new FileNotFoundException($"Recording.pcm not found at {pcmPath}");
        }

        return File.ReadAllBytes(pcmPath);
    }

    private static List<byte[]> SplitIntoChunks(byte[] data, int chunkSize)
    {
        var chunks = new List<byte[]>();

        for (int offset = 0; offset < data.Length; offset += chunkSize)
        {
            int len = Math.Min(chunkSize, data.Length - offset);
            var chunk = new byte[len];
            Array.Copy(data, offset, chunk, 0, len);
            chunks.Add(chunk);
        }

        return chunks;
    }

    private static void ExpectTranscriptionContent(string text)
    {
        var lower = text.ToLowerInvariant();

        string[] keyPhrases =
        [
            "give people",
            "more than one link",
            "live concert",
            "behind the scenes",
            "photo gallery",
            "album to purchase",
        ];

        foreach (var phrase in keyPhrases)
        {
            if (!lower.Contains(phrase))
            {
                Assert.Fail($"Expected transcription to contain '{phrase}'.\nGot: {text}");
            }
        }
    }

    [Test]
    [SkipUnlessIntegration]
    public async Task StreamRecordingWavInChunksAndValidateTranscription()
    {
        if (streamingAudioModel == null || !await streamingAudioModel.IsLoadedAsync())
        {
            throw new SkipTestException("nemotron streaming audio model not available");
        }

        var pcm = LoadRecordingPcm();
        await Assert.That(pcm.Length).IsGreaterThan(0);

        // 100ms chunks at 16kHz mono s16le = 3200 bytes each
        var chunks = SplitIntoChunks(pcm, 3200);
        await Assert.That(chunks.Count).IsGreaterThan(1);

        var audioClient = await streamingAudioModel.GetAudioClientAsync();
        await using var session = audioClient.CreateLiveTranscriptionSession();
        session.Settings.SampleRate = 16000;
        session.Settings.Channels = 1;
        session.Settings.BitsPerSample = 16;

        await session.StartAsync();

        // Start collecting results in background
        var fullText = new System.Text.StringBuilder();
        var readTask = Task.Run(async () =>
        {
            await foreach (var result in session.GetStream())
            {
                if (result.Content != null)
                {
                    foreach (var content in result.Content)
                    {
                        fullText.Append(content.Text);
                    }
                }
            }
        });

        // Stream all chunks
        foreach (var chunk in chunks)
        {
            await session.AppendAsync(new ReadOnlyMemory<byte>(chunk));
        }

        // Stop session to flush and complete
        await session.StopAsync();
        await readTask;

        var text = fullText.ToString();
        await Assert.That(text).IsNotEmpty();
        ExpectTranscriptionContent(text);

        Console.WriteLine($"Streaming transcription: {text}");
    }

    [Test]
    [SkipUnlessIntegration]
    public async Task StreamRecordingWavWithInitialData()
    {
        if (streamingAudioModel == null || !await streamingAudioModel.IsLoadedAsync())
        {
            throw new SkipTestException("nemotron streaming audio model not available");
        }

        var pcm = LoadRecordingPcm();
        await Assert.That(pcm.Length).IsGreaterThan(0);

        // Put first 32000 bytes (~1 second) as initial data, stream the rest
        int initialSize = Math.Min(pcm.Length, 32000);

        var audioClient = await streamingAudioModel.GetAudioClientAsync();
        await using var session = audioClient.CreateLiveTranscriptionSession();
        session.Settings.SampleRate = 16000;
        session.Settings.Channels = 1;
        session.Settings.BitsPerSample = 16;

        await session.StartAsync();

        // Start collecting results
        var fullText = new System.Text.StringBuilder();
        var readTask = Task.Run(async () =>
        {
            await foreach (var result in session.GetStream())
            {
                if (result.Content != null)
                {
                    foreach (var content in result.Content)
                    {
                        fullText.Append(content.Text);
                    }
                }
            }
        });

        // Send initial chunk
        await session.AppendAsync(new ReadOnlyMemory<byte>(pcm, 0, initialSize));

        // Stream the remainder in 100ms chunks
        var remainder = new byte[pcm.Length - initialSize];
        Array.Copy(pcm, initialSize, remainder, 0, remainder.Length);
        var chunks = SplitIntoChunks(remainder, 3200);

        foreach (var chunk in chunks)
        {
            await session.AppendAsync(new ReadOnlyMemory<byte>(chunk));
        }

        await session.StopAsync();
        await readTask;

        var text = fullText.ToString();
        await Assert.That(text).IsNotEmpty();
        ExpectTranscriptionContent(text);

        Console.WriteLine($"Streaming transcription: {text}");
    }

    [Test]
    [SkipUnlessIntegration]
    public async Task EmptyStreamProducesEmptyOrMinimalOutput()
    {
        if (streamingAudioModel == null || !await streamingAudioModel.IsLoadedAsync())
        {
            throw new SkipTestException("nemotron streaming audio model not available");
        }

        var audioClient = await streamingAudioModel.GetAudioClientAsync();
        await using var session = audioClient.CreateLiveTranscriptionSession();
        session.Settings.SampleRate = 16000;
        session.Settings.Channels = 1;
        session.Settings.BitsPerSample = 16;

        await session.StartAsync();

        // Collect results
        var results = new List<LiveAudioTranscriptionResponse>();
        var readTask = Task.Run(async () =>
        {
            await foreach (var result in session.GetStream())
            {
                results.Add(result);
            }
        });

        // Immediately stop without sending any audio data
        await session.StopAsync();
        await readTask;

        // Empty stream may produce empty results or minimal output
        Console.WriteLine($"Empty stream produced {results.Count} results");
    }

    [Test]
    [SkipUnlessIntegration]
    public async Task StreamingCallbackReceivesIntermediateResults()
    {
        if (streamingAudioModel == null || !await streamingAudioModel.IsLoadedAsync())
        {
            throw new SkipTestException("nemotron streaming audio model not available");
        }

        var pcm = LoadRecordingPcm();
        var chunks = SplitIntoChunks(pcm, 3200);

        var audioClient = await streamingAudioModel.GetAudioClientAsync();
        await using var session = audioClient.CreateLiveTranscriptionSession();
        session.Settings.SampleRate = 16000;
        session.Settings.Channels = 1;
        session.Settings.BitsPerSample = 16;

        await session.StartAsync();

        // Collect all intermediate results
        var allResults = new List<LiveAudioTranscriptionResponse>();
        var fullText = new System.Text.StringBuilder();
        var readTask = Task.Run(async () =>
        {
            await foreach (var result in session.GetStream())
            {
                allResults.Add(result);
                if (result.Content != null)
                {
                    foreach (var content in result.Content)
                    {
                        fullText.Append(content.Text);
                    }
                }
            }
        });

        // Stream all chunks
        foreach (var chunk in chunks)
        {
            await session.AppendAsync(new ReadOnlyMemory<byte>(chunk));
        }

        await session.StopAsync();
        await readTask;

        await Assert.That(allResults.Count).IsGreaterThan(0);
        var text = fullText.ToString();
        await Assert.That(text).IsNotEmpty();
        ExpectTranscriptionContent(text);

        Console.WriteLine($"Received {allResults.Count} streaming results");
        Console.WriteLine($"Final transcription: {text}");
    }

    /// <summary>
    /// Streamed input + streamed output: pushes PCM chunks into a session-borrowed
    /// <see cref="ItemQueue"/> while consuming per-token <see cref="SpeechSegmentItem"/>s via
    /// <see cref="Session.ProcessStreamingRequestAsync"/>, then verifies the terminal
    /// <see cref="SpeechResultItem"/> aggregates the same segments. Mirrors the C++
    /// <c>StreamingAudioFixture.StreamingCallbackReceivesTokens</c> test in
    /// <c>sdk_v2/cpp/test/sdk_api/streaming_audio_test.cc</c>. Requires the nemotron
    /// live-streaming ASR model — whisper rejects PCM chunks at inference.
    /// </summary>
    [Test]
    [SkipUnlessIntegration]
    public async Task AudioSession_PcmStreamedInAndSegmentsStreamedOut()
    {
        if (streamingAudioModel == null || !await streamingAudioModel.IsLoadedAsync())
        {
            throw new SkipTestException("nemotron streaming audio model not available");
        }

        var pcm = LoadRecordingPcm();
        await Assert.That(pcm.Length).IsGreaterThan(0);

        // 100ms chunks at 16kHz mono s16le = 3200 bytes each
        var chunks = SplitIntoChunks(pcm, 3200);
        await Assert.That(chunks.Count).IsGreaterThan(1);

        // Format descriptor — no initial data, the actual bytes arrive via the queue.
        // Mirrors the C++ Item::AudioFromData("pcm", nullptr, 0, 16000, 1) shape.
        using var audio = AudioItem.CreateFormatDescriptor("pcm", sampleRate: 16000, channels: 1);
        using var queue = new ItemQueue();

        using var request = new Request();
        request.AddItem(audio, takeOwnership: true);
        request.AddItem(queue, takeOwnership: false);

        using var session = new AudioSession(streamingAudioModel!);
        session.SetStreaming(true);

        // ItemQueue is unbounded, so we can push every chunk synchronously up-front.
        // The native session consumes them on its own worker thread once we kick off
        // ProcessStreamingRequestAsync below.
        foreach (var chunk in chunks)
        {
            // Owned bytes item (State 3, see docs/item-data-ownership.md): the native side takes
            // ownership and its deleter unpins the buffer when the item is destroyed, so there is
            // nothing for the caller to dispose. Generic Push(Item) — no per-type push helper needed.
            queue.Push(BytesItem.CreateOwned(chunk));
        }

        queue.MarkFinished();

        var sb = new System.Text.StringBuilder();
        int segmentCount = 0;

        var stream = session.ProcessStreamingRequestAsync(request);

        await foreach (var item in stream)
        {
            using (item)
            {
                if (item is SpeechSegmentItem seg)
                {
                    sb.Append(seg.Text);
                    segmentCount++;
                }
                else
                {
                    Assert.Fail($"unexpected streamed item type: {item.GetType().Name}");
                }
            }
        }

        await Assert.That(segmentCount).IsGreaterThan(0);
        var streamedText = sb.ToString();
        await Assert.That(streamedText).IsNotEmpty();
        ExpectTranscriptionContent(streamedText);

        // The terminal Response carries an aggregated SpeechResultItem built from the
        // streamed segments. Validate it matches what we accumulated from the iterator.
        using var final = await stream.FinalResponse;
        await Assert.That(final).IsNotNull();
        await Assert.That(final.ItemCount).IsGreaterThan(0);

        SpeechResultItem? aggregated = null;

        for (int i = 0; i < final.ItemCount; i++)
        {
            using var item = final.GetItem(i);

            if (item is SpeechResultItem result)
            {
                aggregated = result;
                break;
            }
        }

        await Assert.That(aggregated).IsNotNull();
        await Assert.That(aggregated!.Segments.Count).IsEqualTo(segmentCount);
        await Assert.That(aggregated.Text).IsEqualTo(streamedText);

        Console.WriteLine($"Streaming transcription ({segmentCount} segments): {streamedText}");
    }
}


