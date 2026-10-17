# Live Audio Transcription Example

Real-time microphone-to-text transcription using the Foundry Local **v2**
JavaScript SDK with a typed `AudioSession`.

## Prerequisites

- Built SDK v2 source package and native runtime (see the [JavaScript samples setup](../README.md))
- Node.js 20+
- A microphone (optional — falls back to synthetic audio)

## Setup

```bash
npm install
```

`naudiodon2` is optional for microphone capture. Without it, the sample pushes
synthetic PCM chunks to exercise the streaming contract.

## Run

```bash
node app.js
```

## How it works

1. Loads `nemotron-speech-streaming-en-0.6b`
2. Creates a typed `AudioSession`
3. Builds a request with:
   - an audio format descriptor: `Item.audioDescriptor("pcm", 16000, 1)`
   - a borrowed `ItemQueue` carrying **`Item.bytes(...)` entries only**
4. Streams audio chunks from `naudiodon2` (or synthetic fallback)
5. Prints incremental `speechSegment` deltas as they arrive
6. Reads the terminal `speechResult` from `stream.response`

## Important streaming behavior

Current Whisper and Nemotron streaming models emit **`SpeechSegmentKind.None`**
for token deltas. Future models may also emit `Partial` and `Final`.

- `None`: print as incremental text delta
- `Partial`: print as a cumulative hypothesis
- `Final`: treat as a stabilized segment if a future model emits it
- final `speechResult`: full transcript for the whole request

Do **not** push `audio` items into the queue. The queue payloads must be raw
bytes items; the audio format is declared once by the descriptor item on the
request.
