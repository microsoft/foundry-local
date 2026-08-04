// Live Audio Transcription — Foundry Local SDK Example (native session API)
//
// Uses AudioSession + ItemQueue + ProcessStreamingRequestAsync directly.
// NAudio's WaveInEvent is Windows-only. On non-Windows platforms, the sample
// falls back to synthetic PCM audio.

using Microsoft.AI.Foundry.Local;
using NAudio.Wave;

Console.WriteLine("===========================================================");
Console.WriteLine("   Foundry Local -- Live Audio Transcription Demo");
Console.WriteLine("===========================================================");
Console.WriteLine();

var config = new Configuration
{
    AppName = "foundry_local_samples",
    LogLevel = Microsoft.AI.Foundry.Local.LogLevel.Information
};

await FoundryLocalManager.CreateAsync(config, Utils.GetAppLogger());
var mgr = FoundryLocalManager.Instance;

await Utils.RunWithSpinner("Registering execution providers", mgr.DownloadAndRegisterEpsAsync());

var catalog = await mgr.GetCatalogAsync();

// English-only:
var modelAlias = "nemotron-speech-streaming-en-0.6b";
// Multi-lingual (supports 30+ languages including auto-detect):
// var modelAlias = "nemotron-3.5-asr-streaming-0.6b";

var model = await catalog.GetModelAsync(modelAlias) ?? throw new Exception($"Model \"{modelAlias}\" not found in catalog");

await model.DownloadAsync(progress =>
{
    Console.Write($"\rDownloading model: {progress:F2}%");
    if (progress >= 100f)
    {
        Console.WriteLine();
    }
});

Console.Write($"Loading model {model.Id}...");
await model.LoadAsync();
Console.WriteLine("done.");

const int SampleRate = 16000;
const int Channels = 1;
// Multi-lingual examples:
// session.Settings.Language = "de";     // German
// session.Settings.Language = "zh-CN";  // Chinese (Simplified)
// session.Settings.Language = "auto";   // Auto-detect language
// English (default)
const string Language = "en";                  

bool useSynth = args.Contains("--synth");

// Scope session in a block so it is disposed before model.UnloadAsync() (the model's session
// refcount must drop to zero before unload).
{
    using var session = new AudioSession(model);
    session.SetOptions(new RequestOptions
    {
        AdditionalOptions = new Dictionary<string, string> { ["language"] = Language },
    });
    session.SetStreaming(true);

    // ItemQueue: live audio chunks get pushed in as BytesItems while ProcessStreamingRequestAsync runs.
    using var audioQueue = new ItemQueue();

    using var request = new Request();
    // 1) Audio format descriptor (no data) tells the session how to interpret subsequent chunks.
    request.AddItem(AudioItem.CreateFormatDescriptor("pcm", SampleRate, Channels));
    // 2) The streaming input queue. We retain ownership and push chunks until MarkFinished.
    request.AddItem(audioQueue, takeOwnership: false);

    Console.WriteLine("       Session started");

    // Start the streaming request. We keep the StreamingResponse around so we can
    // await FinalResponse after streaming drains — it carries the aggregated
    // transcription as a SpeechResultItem.
    var streaming = session.ProcessStreamingRequestAsync(request);

    // Background reader: stream transcription items as they arrive.
    var readTask = Task.Run(async () =>
    {
        try
        {
            await foreach (var item in streaming)
            {
                using (item)
                {
                    if (item is SpeechSegmentItem segment && !string.IsNullOrEmpty(segment.Text))
                    {
                        Console.ForegroundColor = ConsoleColor.Cyan;
                        Console.Write(segment.Text);
                        Console.ResetColor();
                        Console.Out.Flush();
                    }
                }
            }
        }
        catch (OperationCanceledException) { }
    });

    // NAudio WaveInEvent is Windows-only. On other platforms, fall back to synthetic audio.
    if (!useSynth && OperatingSystem.IsWindows())
    {
        using var waveIn = new WaveInEvent
        {
            WaveFormat = new WaveFormat(rate: SampleRate, bits: 16, channels: Channels),
            BufferMilliseconds = 100
        };

        // Bounded channel: NAudio's DataAvailable callback is synchronous, so we enqueue PCM
        // chunks and push them into the SDK queue on a dedicated task.
        var audioChannel = System.Threading.Channels.Channel.CreateBounded<byte[]>(
            new System.Threading.Channels.BoundedChannelOptions(50)
            {
                FullMode = System.Threading.Channels.BoundedChannelFullMode.DropOldest
            });

        var appendTask = Task.Run(async () =>
        {
            await foreach (var chunk in audioChannel.Reader.ReadAllAsync())
            {
                audioQueue.Push(BytesItem.CreateOwned(chunk));
            }
        });

        waveIn.DataAvailable += (sender, e) =>
        {
            if (e.BytesRecorded > 0)
            {
                var buffer = new byte[e.BytesRecorded];
                Buffer.BlockCopy(e.Buffer, 0, buffer, 0, e.BytesRecorded);
                audioChannel.Writer.TryWrite(buffer);
            }
        };

        Console.WriteLine();
        Console.WriteLine("===========================================================");
        Console.WriteLine("  LIVE TRANSCRIPTION ACTIVE");
        Console.WriteLine("  Speak into your microphone.");
        Console.WriteLine("  Transcription appears in real-time (cyan text).");
        Console.WriteLine("  Press ENTER to stop recording.");
        Console.WriteLine("===========================================================");
        Console.WriteLine();

        waveIn.StartRecording();
        Console.ReadLine();
        waveIn.StopRecording();

        audioChannel.Writer.Complete();
        await appendTask;
    }
    else
    {
        if (!OperatingSystem.IsWindows() && !useSynth)
        {
            Console.WriteLine("NAudio mic capture is Windows-only. Falling back to synthetic audio...");
        }

        // Synthetic PCM fallback: 440Hz sine wave, 2 seconds
        Console.WriteLine("Pushing synthetic audio (440Hz sine, 2s)...");
        const int duration = 2;
        var totalSamples = SampleRate * duration;
        var pcmBytes = new byte[totalSamples * 2];
        for (int i = 0; i < totalSamples; i++)
        {
            double t = (double)i / SampleRate;
            short sample = (short)(short.MaxValue * 0.5 * Math.Sin(2 * Math.PI * 440 * t));
            pcmBytes[i * 2] = (byte)(sample & 0xFF);
            pcmBytes[i * 2 + 1] = (byte)((sample >> 8) & 0xFF);
        }

        int chunkSize = (SampleRate / 10) * 2; // 100ms
        for (int offset = 0; offset < pcmBytes.Length; offset += chunkSize)
        {
            int len = Math.Min(chunkSize, pcmBytes.Length - offset);
            var chunk = new byte[len];
            Buffer.BlockCopy(pcmBytes, offset, chunk, 0, len);
            audioQueue.Push(BytesItem.CreateOwned(chunk));
            await Task.Delay(100);
        }

        Console.WriteLine("✓ Synthetic audio pushed");
    }

    // Signal end-of-input; the streaming session will drain remaining items and complete.
    audioQueue.MarkFinished();
    await readTask;
    Console.WriteLine();

    // FinalResponse carries the aggregated transcription as a single SpeechResultItem.
    using var finalResponse = await streaming.FinalResponse;
    using var finalItem = finalResponse.GetItem(0);
    var transcript = (finalItem as SpeechResultItem)?.Text ?? string.Empty;

    Console.WriteLine();
    Console.WriteLine("===========================================================");
    Console.WriteLine("  FINAL TRANSCRIPTION");
    Console.WriteLine("===========================================================");
    Console.WriteLine(transcript);
}

await model.UnloadAsync();
mgr.Dispose();
