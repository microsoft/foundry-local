# Live Audio Transcription Example

Real-time microphone-to-text transcription using the Foundry Local **v2**
Python SDK with a typed `AudioSession`.

## Prerequisites

- SDK v2 source package and native runtime (see the [Python samples setup](../README.md))
- Python 3.11+
- A microphone (optional — falls back to synthetic audio with `--synth` or if
  PyAudio is unavailable)

## Setup

```bash
pip install -r requirements.txt
```

`pyaudio` is optional for live microphone capture.

## Run

```bash
python src/app.py
python src/app.py --synth
```

## How it works

1. Loads `nemotron-speech-streaming-en-0.6b`
2. Creates a typed `AudioSession`
3. Builds a request with:
   - `AudioItem.create_format_descriptor("pcm", sample_rate=16000, channels=1)`
   - a borrowed `ItemQueue` carrying **`BytesItem` PCM chunks only**
4. Streams microphone or synthetic audio into the queue
5. Prints incremental `SpeechSegmentItem` deltas as they arrive
6. Reads the terminal `SpeechResultItem` from `stream.final_response`

## Important streaming behavior

Current Whisper and Nemotron streaming models emit **`SpeechSegmentKind.NONE`**
for token deltas. Future models may also emit `PARTIAL` and `FINAL`.

- `NONE`: print as incremental text delta
- `PARTIAL`: print as a cumulative hypothesis
- `FINAL`: treat as a stabilized segment if a future model emits it
- final `SpeechResultItem`: full transcript for the whole request

Do **not** push `AudioItem` instances into the queue. The queue payloads must
be `BytesItem` chunks; the audio format is declared once by the descriptor item
on the request.
