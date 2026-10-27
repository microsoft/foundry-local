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

Speak into your microphone. Transcription appears in real-time. Press `Ctrl+C` to stop.

To force synthetic audio (e.g., for CI or when no microphone is available):

```bash
python src/app.py --synth
```

## How it works

1. Initializes the Foundry Local SDK and loads the Nemotron ASR model
2. Creates a `LiveAudioTranscriptionSession` with 16kHz/16-bit/mono PCM settings
3. Captures microphone audio via `pyaudio` (or generates synthetic audio as fallback)
4. Pushes PCM chunks to the SDK via `session.append()`
5. Reads transcription results in a background thread via `for result in session.get_stream()`
6. Access text via `result.content[0].text` (OpenAI Realtime ConversationItem pattern)

## API

```python
audio_client = model.get_audio_client()
session = audio_client.create_live_transcription_session()
session.settings.sample_rate = 16000
session.settings.channels = 1
session.settings.language = "en"

session.start()

# Push audio
session.append(pcm_bytes)

# Read results (typically on a background thread)
for result in session.get_stream():
    print(result.content[0].text)        # transcribed text
    print(result.content[0].transcript)  # alias (OpenAI compat)
    print(result.is_final)               # True for final results

session.stop()
session.close()
```
