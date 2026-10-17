# Live Audio Transcription Example

Real-time microphone-to-text transcription using the Foundry Local C# SDK with Nemotron ASR.

## Prerequisites

- SDK v2 and its matching native runtime (see [shared setup](../README.md))
- .NET 9 SDK
- A microphone (optional — falls back to synthetic audio on non-Windows or with `--synth`)

## Setup

```bash
dotnet restore
```

> **Note:** Microphone capture uses [NAudio](https://github.com/naudio/NAudio) and is Windows-only. On other platforms, the sample falls back to synthetic audio for testing.

## Run

```bash
dotnet run
```

Speak into your microphone. Transcription tokens appear in real time. Press `ENTER` to stop recording.

To force synthetic audio (e.g., for CI or non-Windows):

```bash
dotnet run -- --synth
```

## How it works

1. Initializes the Foundry Local SDK and loads the Nemotron ASR model
2. Creates an `AudioSession` with streaming enabled and an input `ItemQueue`
3. Captures microphone audio via `NAudio.WaveInEvent` (or generates synthetic audio as fallback)
4. Pushes owned 16kHz/16-bit/mono PCM `BytesItem` chunks into the queue through a bounded capture channel
5. Reads `SpeechSegmentItem` results from `ProcessStreamingRequestAsync`
6. Marks the queue finished, awaits the final `SpeechResultItem`, and disposes the session before unloading the model

Current models emit token deltas with `SpeechSegmentKind.None`. A `Partial`
segment, if supplied by a model, is a cumulative hypothesis rather than a delta.

## API

```csharp
using var session = new AudioSession(model);
session.SetStreaming(true);
using var queue = new ItemQueue();
using var request = new Request();
request.AddItem(AudioItem.CreateFormatDescriptor("pcm", 16000, 1));
request.AddItem(queue, takeOwnership: false);
await using var stream = session.ProcessStreamingRequestAsync(request);

using var audio = BytesItem.CreateOwned(pcmBytes.AsMemory());
queue.Push(audio);
queue.MarkFinished();

await foreach (var item in stream)
{
    using (item)
    {
        if (item is SpeechSegmentItem segment)
        {
            Console.WriteLine($"{segment.Kind}: {segment.Text}");
        }
    }
}
using var response = await stream.FinalResponse;
Console.WriteLine(response.OfType<SpeechResultItem>().Single().Text);
```
