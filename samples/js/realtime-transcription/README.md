# Realtime transcription

This sample captures 16 kHz, mono, signed 16-bit PCM from the default microphone with `naudiodon2`. It sends the
audio through an `ItemQueue` while consuming native `AudioSession` speech-segment output.

```bash
npm install
npm start
```

Speak into the microphone and press Enter to stop. The default model alias is
`nemotron-speech-streaming-en-0.6b`; pass another automatic-speech-recognition model alias as the first argument.

`naudiodon2` builds a native PortAudio module during installation, so the platform compiler and PortAudio
prerequisites documented by that package must be available.
