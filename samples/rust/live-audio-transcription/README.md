# Live Audio Transcription Example (Rust)

Real-time microphone-to-text transcription using the Foundry Local **v2**
Rust SDK with a typed `AudioSession`.

Uses CPAL for optional microphone capture and falls back to synthetic PCM.

## Run

```bash
cd samples/rust
cargo run -p live-audio-transcription
cargo run -p live-audio-transcription -- --synth
```

## How it works

1. Loads `nemotron-speech-streaming-en-0.6b`
2. Creates a typed `AudioSession`
3. Creates a request with:
   - an audio descriptor via `Item::audio_data(Vec::<u8>::new(), Some("pcm"), 16000, 1)`
   - a borrowed input queue via `Request::with_input_queue(...)`
4. Pushes **`Item::bytes(...)` PCM chunks only** into the queue from CPAL or a synthetic sine wave
5. Prints incremental `Item::SpeechSegment` deltas while streaming
6. Reads the terminal `Item::SpeechResult` from `stream.response().await`

## Important streaming behavior

Current Whisper and Nemotron streaming models emit **`SpeechSegmentKind::None`**
for token deltas. Future models may also emit `Partial` and `Final`.

- `None`: print as incremental text delta
- `Partial`: print as a cumulative hypothesis
- `Final`: treat as a stabilized segment if a future model emits it
- final `SpeechResult`: full transcript for the whole request

Do **not** push `Item::Audio` payloads into the queue. The request declares the
audio format once up front; the queue itself must carry bytes items only.
