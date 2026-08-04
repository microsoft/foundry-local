# Live Audio Transcription Example

Real-time microphone-to-text transcription using the Foundry Local Python SDK with Nemotron ASR.

## Prerequisites

- [Foundry Local](https://github.com/microsoft/Foundry-Local) installed
- Python 3.9+
- A microphone (optional — falls back to synthetic audio with `--synth` or if PyAudio is unavailable)

## Setup

```bash
pip install -r requirements.txt
```

> **Note:** `pyaudio` is **optional** — it provides cross-platform microphone capture. Without it, the example falls back to synthetic audio for testing.
>
> Install manually if needed:
> ```bash
> pip install pyaudio
> ```

## Run

```bash
python src/app.py
```

Speak into your microphone. Transcription appears in real-time. Press `ENTER` to stop.

To force synthetic audio (e.g., for CI or when no microphone is available):

```bash
python src/app.py --synth
```

## How it works

1. Initializes the Foundry Local SDK and loads the Nemotron ASR model
2. Opens an `AudioSession` configured for streaming
3. Builds a `Request` containing an `AudioItem` format descriptor (16 kHz mono PCM)
   and a caller-owned `ItemQueue` to feed live PCM chunks into
4. Captures microphone audio via `pyaudio` (or generates synthetic audio as fallback) and
   pushes each chunk as a `BytesItem` into the queue
5. Reads transcription `SpeechSegmentItem`s in a background thread via `session.process_streaming_request(request)`
6. Calls `audio_queue.mark_finished()` to signal end-of-input; the session drains and the reader exits

## API

```python
from foundry_local_sdk import (
    AudioItem, AudioSession, BytesItem, ItemQueue,
    Request, RequestOptions, SpeechSegmentItem,
)

with AudioSession(model) as session:
    session.set_options(RequestOptions(additional_options={"language": "en"}))
    session.set_streaming(True)

    with ItemQueue() as audio_queue, Request() as request:
        # Format descriptor (no data) + the streaming input queue.
        request.add_item(AudioItem.create_format_descriptor("pcm", 16000, 1))
        request.add_item(audio_queue, transfer_ownership=False)

        # On a background thread: consume transcription items as they arrive.
        for item in session.process_streaming_request(request):
            if isinstance(item, SpeechSegmentItem):
                print(item.text, end="", flush=True)

        # On the producer thread: push PCM chunks, then signal end-of-input.
        audio_queue.push(BytesItem(pcm_bytes))
        audio_queue.mark_finished()
```
