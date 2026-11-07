# Realtime transcription

Captures 16 kHz mono PCM from the default microphone and feeds it through an
`ItemQueue` to the native `AudioSession`. Transcription segments are yielded by
the native streaming callback while audio is still arriving.

```bash
python -m pip install -r requirements.txt
python src/app.py --seconds 15
```

The `sounddevice` package requires PortAudio. Grant microphone permission when
your operating system prompts for it.
