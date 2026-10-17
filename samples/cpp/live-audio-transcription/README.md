# Live Audio Transcription Example (C++)

Real-time microphone-to-text transcription using the Foundry Local **v2**
C++ SDK with a typed `AudioSession`.

Uses PortAudio for optional microphone capture and falls back to synthetic PCM.

## Build

Build the [native SDK v2 runtime](../../../sdk_v2/cpp/README.md) first. On Windows,
run the following from the repository root in a Visual Studio developer shell.
The executable is placed beside the matching runtime DLLs:

```powershell
cl /nologo /std:c++20 /EHsc /I sdk_v2\cpp\include /Fo:sdk_v2\cpp\build\live-audio.obj /Fe:sdk_v2\cpp\build\Windows\RelWithDebInfo\bin\RelWithDebInfo\live-audio.exe samples\cpp\live-audio-transcription\main.cpp sdk_v2\cpp\build\Windows\RelWithDebInfo\RelWithDebInfo\foundry_local.lib
sdk_v2\cpp\build\Windows\RelWithDebInfo\bin\RelWithDebInfo\live-audio.exe --synth
```

On Linux or macOS, set `FOUNDRY_LOCAL_INCLUDE_DIR` to `sdk_v2/cpp/include` and
`FOUNDRY_LOCAL_LIB_DIR` to the native build's binary directory, using absolute paths:

```bash
# With PortAudio (live microphone)
g++ -std=c++20 -DHAS_PORTAUDIO main.cpp -I "$FOUNDRY_LOCAL_INCLUDE_DIR" -L "$FOUNDRY_LOCAL_LIB_DIR" -Wl,-rpath,"$FOUNDRY_LOCAL_LIB_DIR" -lfoundry_local -lportaudio -o live-audio-transcription-example

# Without PortAudio (synthetic audio only)
g++ -std=c++20 main.cpp -I "$FOUNDRY_LOCAL_INCLUDE_DIR" -L "$FOUNDRY_LOCAL_LIB_DIR" -Wl,-rpath,"$FOUNDRY_LOCAL_LIB_DIR" -lfoundry_local -o live-audio-transcription-example
```

## Run

```bash
./live-audio-transcription-example
./live-audio-transcription-example --synth
```

## How it works

1. Loads `nemotron-speech-streaming-en-0.6b`
2. Creates a typed `AudioSession`
3. Builds a request with:
   - `Item::AudioFromData("pcm", nullptr, 0, 16000, 1)` as the format descriptor
   - a borrowed `ItemQueue` for streamed **`Item::Bytes(...)` PCM chunks only**
4. Streams microphone or synthetic PCM into the queue
5. Prints incremental `SpeechSegment` deltas via `SetStreamingCallback`
6. Reads the terminal `SpeechResult` from the final `Response`

## Important streaming behavior

Current Whisper and Nemotron streaming models emit **`SpeechSegmentKind.None`**
for token deltas. Future models may also emit partial/final segments.

- current `None` segments: print as incremental deltas
- future partial/final segments: handle as cumulative/stabilized hypotheses
- final `SpeechResult`: full transcript for the whole request

Do **not** push `Audio` items into the queue. The descriptor item defines the
PCM format once; queue entries must be raw bytes items.
