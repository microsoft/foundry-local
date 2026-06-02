# Live Audio Transcription Example

Real-time microphone-to-text transcription using the Foundry Local C# SDK with Nemotron ASR.

## Prerequisites

- [Foundry Local](https://github.com/microsoft/Foundry-Local) installed
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

Speak into your microphone. Transcription appears in real-time (cyan text). Press `ENTER` to stop recording.

To force synthetic audio (e.g., for CI or non-Windows):

```bash
dotnet run -- --synth
```

## How it works

1. Initializes the Foundry Local SDK and loads the Nemotron ASR model
2. Opens an `AudioSession` configured for streaming
3. Builds a `Request` containing an `AudioItem` format descriptor (16 kHz mono PCM)
   and an `ItemQueue` to feed live PCM chunks into
4. Captures microphone audio via `NAudio.WaveInEvent` (or generates synthetic audio as fallback)
   and pushes each chunk as a `BytesItem` into the queue (through a bounded channel for backpressure)
5. Reads transcription `TextItem`s via `await foreach (var item in session.ProcessStreamingRequestAsync(request))`
6. Calls `audioQueue.MarkFinished()` to signal end-of-input; the session drains and the loop exits

## API

```csharp
using var session = new AudioSession(model);
session.SetOptions(new RequestOptions
{
    AdditionalOptions = { ["language"] = "en" },
});
session.SetStreaming(true);

using var audioQueue = new ItemQueue();
using var request = new Request();

// Format descriptor (no data) + the streaming input queue.
request.AddItem(AudioItem.CreateFormatDescriptor("pcm", 16000, 1));
request.AddItem(audioQueue, takeOwnership: false);

// On a background task: consume transcription items as they arrive.
await foreach (var item in session.ProcessStreamingRequestAsync(request))
{
    if (item is TextItem text)
    {
        Console.Write(text.Text);
    }
}

// On the producer side: push PCM chunks, then signal end-of-input.
audioQueue.Push(BytesItem.CreateOwned(pcmBytes));
audioQueue.MarkFinished();
```
