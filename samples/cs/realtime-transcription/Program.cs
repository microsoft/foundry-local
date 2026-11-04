using System.Threading.Channels;
using Microsoft.AI.Foundry.Local;
using Microsoft.Extensions.Logging.Abstractions;
using NAudio.Wave;

if (!OperatingSystem.IsWindows())
{
    Console.Error.WriteLine("This microphone sample currently requires Windows because it uses NAudio WaveInEvent.");
    return;
}

var configuration = new Configuration
{
    AppName = "foundry_local_samples",
    LogLevel = LogLevel.Information,
};

await FoundryLocalManager.CreateAsync(configuration, NullLogger.Instance).ConfigureAwait(false);
using var manager = FoundryLocalManager.Instance;
await manager.DownloadAndRegisterEpsAsync().ConfigureAwait(false);

var catalog = await manager.GetCatalogAsync().ConfigureAwait(false);
var model = await catalog.GetModelAsync("nemotron-speech-streaming-en-0.6b").ConfigureAwait(false)
    ?? throw new InvalidOperationException("The streaming transcription model was not found.");

if (!await model.IsCachedAsync().ConfigureAwait(false))
{
    Console.WriteLine($"Downloading {model.Id}...");
    await model.DownloadAsync(
        progress => Console.Write($"\r  {progress:F1}%   "))
        .ConfigureAwait(false);
    Console.WriteLine();
}

await model.LoadAsync().ConfigureAwait(false);

try
{
    using var session = new AudioSession(model);
    session.SetStreaming(true);
    session.SetOptions(new RequestOptions
    {
        AdditionalOptions = new Dictionary<string, string> { ["language"] = "en" },
    });

    using var audioQueue = new ItemQueue();
    using var request = new Request();
    request.AddItem(AudioItem.CreateFormatDescriptor("pcm", sampleRate: 16000, channels: 1));
    request.AddItem(audioQueue, takeOwnership: false);

    var capturedAudio = Channel.CreateBounded<byte[]>(
        new BoundedChannelOptions(50)
        {
            SingleReader = true,
            SingleWriter = true,
            FullMode = BoundedChannelFullMode.DropOldest,
        });

    var forwardAudioTask = Task.Run(async () =>
    {
        try
        {
            await foreach (var chunk in capturedAudio.Reader.ReadAllAsync().ConfigureAwait(false))
            {
                audioQueue.Push(BytesItem.CreateOwned(chunk));
            }
        }
        finally
        {
            audioQueue.MarkFinished();
        }
    });

    var stream = session.ProcessStreamingRequestAsync(request);
    await using (stream)
    {
        var transcriptionTask = Task.Run(async () =>
        {
            await foreach (var item in stream.ConfigureAwait(false))
            {
                using (item)
                {
                    if (item is SpeechSegmentItem segment)
                    {
                        Console.Write(segment.Text);
                        Console.Out.Flush();
                    }
                }
            }
        });

        try
        {
            using var microphone = new WaveInEvent
            {
                WaveFormat = new WaveFormat(rate: 16000, bits: 16, channels: 1),
                BufferMilliseconds = 100,
            };

            microphone.DataAvailable += (_, eventArgs) =>
            {
                if (eventArgs.BytesRecorded == 0)
                {
                    return;
                }

                var chunk = new byte[eventArgs.BytesRecorded];
                Buffer.BlockCopy(eventArgs.Buffer, 0, chunk, 0, chunk.Length);
                capturedAudio.Writer.TryWrite(chunk);
            };

            Console.WriteLine("Listening. Speak into your microphone, then press Enter to stop.");
            microphone.StartRecording();
            Console.ReadLine();
            microphone.StopRecording();
        }
        finally
        {
            capturedAudio.Writer.TryComplete();
            await forwardAudioTask.ConfigureAwait(false);
            await transcriptionTask.ConfigureAwait(false);
        }

        using var finalResponse = await stream.FinalResponse.ConfigureAwait(false);
        foreach (var item in finalResponse)
        {
            using (item)
            {
                if (item is SpeechResultItem result)
                {
                    Console.WriteLine();
                    Console.WriteLine($"Final transcript: {result.Text}");
                }
            }
        }
    }
}
finally
{
    await model.UnloadAsync().ConfigureAwait(false);
}
