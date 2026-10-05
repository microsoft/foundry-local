// Live Audio Transcription — Foundry Local SDK Example
//
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

var audioClient = await model.GetAudioClientAsync();
var session = audioClient.CreateLiveTranscriptionSession();
session.Settings.SampleRate = 16000;  // Default is 16000; shown here to match the NAudio WaveFormat below
session.Settings.Channels = 1;
session.Settings.Language = "en";                  // English (default)
// Multi-lingual examples:
// session.Settings.Language = "de";     // German
// session.Settings.Language = "zh-CN";  // Chinese (Simplified)
// session.Settings.Language = "auto";   // Auto-detect language

await session.StartAsync();
Console.WriteLine("       Session started");

var readTask = Task.Run(async () =>
{
    try
    {
        await foreach (var result in session.GetStream())
        {
            var text = result.Content?[0]?.Text;
            if (result.IsFinal)
            {
                Console.WriteLine();
                Console.WriteLine($"  [FINAL] {text}");
                Console.Out.Flush();
            }
            else if (!string.IsNullOrEmpty(text))
            {
                Console.ForegroundColor = ConsoleColor.Cyan;
                Console.Write(text);
                Console.ResetColor();
                Console.Out.Flush();
            }
        }
    }
    catch (OperationCanceledException) { }
});

bool useSynth = args.Contains("--synth");

// NAudio WaveInEvent is Windows-only. On other platforms, fall back to synthetic audio.
if (!useSynth && OperatingSystem.IsWindows())
{
    using var waveIn = new WaveInEvent
    {
        WaveFormat = new WaveFormat(rate: 16000, bits: 16, channels: 1),
        BufferMilliseconds = 100
    };

    // Use a bounded channel to avoid unbounded fire-and-forget AppendAsync calls.
    // NAudio's DataAvailable callback is synchronous, so we enqueue PCM chunks and
    // await AppendAsync on a dedicated task to respect SDK backpressure.
    var audioChannel = System.Threading.Channels.Channel.CreateBounded<byte[]>(
        new System.Threading.Channels.BoundedChannelOptions(50)
        {
            FullMode = System.Threading.Channels.BoundedChannelFullMode.DropOldest
        });

    var appendTask = Task.Run(async () =>
    {
        await foreach (var chunk in audioChannel.Reader.ReadAllAsync())
        {
            await session.AppendAsync(chunk);
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
    const int sampleRate = 16000;
    const int duration = 2;
    var totalSamples = sampleRate * duration;
    var pcmBytes = new byte[totalSamples * 2];
    for (int i = 0; i < totalSamples; i++)
    {
        double t = (double)i / sampleRate;
        short sample = (short)(short.MaxValue * 0.5 * Math.Sin(2 * Math.PI * 440 * t));
        pcmBytes[i * 2] = (byte)(sample & 0xFF);
        pcmBytes[i * 2 + 1] = (byte)((sample >> 8) & 0xFF);
    }

    int chunkSize = (sampleRate / 10) * 2; // 100ms
    for (int offset = 0; offset < pcmBytes.Length; offset += chunkSize)
    {
        int len = Math.Min(chunkSize, pcmBytes.Length - offset);
        await session.AppendAsync(pcmBytes.AsMemory(offset, len));
        await Task.Delay(100);
    }

    Console.WriteLine("✓ Synthetic audio pushed");
    await Task.Delay(3000); // Wait for remaining transcription results
}

await session.StopAsync();
await readTask;
await session.DisposeAsync();

await model.UnloadAsync();
