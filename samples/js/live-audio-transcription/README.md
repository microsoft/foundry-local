# Live Audio Transcription Example

Real-time microphone-to-text transcription using the Foundry Local JS SDK with Nemotron ASR.

## Prerequisites

- [Foundry Local](https://github.com/microsoft/Foundry-Local) installed
- Node.js 18+
- A microphone (optional — falls back to synthetic audio)

## Setup

```bash
npm install
```

> **Note:** `naudiodon2` is optional — provides cross-platform microphone capture. Without it, the example falls back to synthetic audio for testing.

## Run

```bash
node app.js
```

Speak into your microphone. Transcription appears in real-time (cyan text). Press `ENTER` to stop.

To force synthetic audio (e.g., for CI or when no microphone is available):

```bash
node app.js --synth
```

## How it works

1. Initializes the Foundry Local SDK and loads the Nemotron ASR model
2. Opens an `AudioSession` configured with language options
3. Builds a `Request` containing an `Item.audioDescriptor('pcm', 16000, 1)` and a caller-owned
   `ItemQueue` to feed live PCM chunks into
4. Captures microphone audio via `naudiodon2` (or generates synthetic audio as fallback) and
   pushes each chunk as `Item.bytes(...)` into the queue
5. Reads transcription `TextItem`s in a background async iterator via
   `for await (const item of session.processStreamingRequest(request))`
6. Calls `audioQueue.markFinished()` to signal end-of-input; the session drains and the loop exits

## API

```javascript
import { AudioSession, Item, ItemQueue, Request } from 'foundry-local-sdk';

const session = new AudioSession(model);
session.setOptions({ additionalOptions: { language: 'en' } });

const audioQueue = new ItemQueue();
const request = new Request();
// Format descriptor (no data) + the streaming input queue.
request.addItem(Item.audioDescriptor('pcm', 16000, 1));
request.addItem(audioQueue);

// Background reader: consume transcription items as they arrive.
const readPromise = (async () => {
    for await (const item of session.processStreamingRequest(request)) {
        if (item.type === 'text') {
            process.stdout.write(item.text);
        }
    }
})();

// Producer side: push PCM chunks, then signal end-of-input.
audioQueue.push(Item.bytes(pcmBytes));
audioQueue.markFinished();
await readPromise;

audioQueue.dispose();
session.dispose();
```
