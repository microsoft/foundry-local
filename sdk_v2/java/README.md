# Foundry Local Java SDK

Java 17+ bindings for in-process, streaming speech recognition through the
Foundry Local C API. The SDK uses JNA, keeps the native runtime and model
weights outside the JAR, and exposes deterministic `AutoCloseable` lifetimes.

This package is a preview and is not published to Maven Central yet.

## Build

Prerequisites:

- JDK 17 or newer
- Maven 3.9 or newer

```powershell
mvn -f sdk_v2\java\pom.xml package
```

The build produces:

- `target/foundry-local-sdk-0.1.0-SNAPSHOT.jar`
- `target/foundry-local-sdk-0.1.0-SNAPSHOT-sources.jar`

Override `revision` when producing an immutable release:

```powershell
mvn -f sdk_v2\java\pom.xml -Drevision=0.1.0 package
```

JNA remains a normal Maven dependency. The SDK JAR does not contain JNA native
code, Foundry Local native libraries, execution providers, or model weights.

## Runtime setup

Prepare a directory containing the matching Foundry Local native runtime for
the current OS and architecture. Pass that directory through `Configuration`.
The Java binding requests the stable API v1 prefix, which the current v2 C API
keeps ABI-compatible.

The runtime directory must contain:

- Windows: `foundry_local.dll`
- Linux: `libfoundry_local.so`
- macOS: `libfoundry_local.dylib`

When ONNX Runtime or ONNX Runtime GenAI libraries are present in the same
directory, the SDK preloads them before Foundry Local. Otherwise, the platform
loader must be able to resolve those dependencies.

The platform-independent JAR can run wherever the matching Foundry Local native
runtime is available. Current upstream native artifacts target Windows x64 and
ARM64, Linux x64 and ARM64, and macOS ARM64. Use a 64-bit JVM with the same
architecture as the native runtime.

On Java 25, pass `--enable-native-access=ALL-UNNAMED` when required by the JVM.
The older JBR 21.0.8 and 21.0.9 builds tested with Runtime 2.0.1 on Windows load
an older C runtime first and cannot initialize ONNX Runtime. Use a compatible
JBR/native-runtime combination instead of replacing IDE or system DLLs.

## Streaming ASR

The model is loaded once and reused across successive dictation requests.
Each `AudioSession` accepts one active `Transcription`; close it before starting
the next request.

```java
var configuration = new Configuration(
        "my-app",
        Path.of("native-runtime"),
        Path.of("model-cache"),
        Path.of("app-data"));

try (var manager = new FoundryLocalManager(configuration)) {
    var model = manager.catalog()
            .getModel("nemotron-3.5-asr-streaming-0.6b-generic-cpu:3");

    if (!model.isCached()) {
        throw new IllegalStateException("Download and review the model separately");
    }

    model.load();
    try (var session = model.createAudioSession()) {
        for (Iterable<byte[]> request : requests) {
            try (var transcription =
                    session.streamPcm(PcmFormat.SPEECH, event -> showInterim(event.text()))) {
                for (byte[] pcmChunk : request) {
                    // signed PCM16LE, 16 kHz, mono
                    transcription.writePcm(pcmChunk);
                }
                transcription.finishInput();
                TranscriptionResult result = transcription.await();
                useFinalText(result.text());
            }
        }
    } finally {
        model.unload();
    }
}
```

For a real microphone stream, create one `Transcription`, call `writePcm` for
each chunk, then call `finishInput` and `await`. `finishInput` drains queued
audio and publishes the final transcript. `cancel` requests native cancellation
and produces a result with `cancelled() == true`.

PCM input is intentionally limited to signed PCM16LE, 16 kHz, mono. Each call
accepts at most one second of audio, and the SDK applies backpressure after two
seconds are queued.

## Ownership and threading

- `FoundryLocalManager` owns catalogs, models, and sessions. Only one manager
  may be open at a time, but a manager can be closed and recreated later.
- A loaded `Model` can create multiple successive `AudioSession` instances
  without reloading the model.
- Closing a manager closes outstanding sessions and transcriptions before
  releasing the native manager.
- Download progress and speech listeners run on native callback threads. Keep
  callbacks short and do not call SDK lifecycle or input methods from them.
- `Transcription.close()` cancels unfinished work and waits for native callbacks
  before releasing request and PCM buffers.

## Tests

```powershell
mvn -f sdk_v2\java\pom.xml test
```

The native ASR integration test is opt-in and never downloads a model:

```powershell
mvn -f sdk_v2\java\pom.xml test `
  -Dtest=NativeAsrTest `
  -Dfoundry.test.runtime=<absolute-native-runtime-dir> `
  -Dfoundry.test.cache=<absolute-model-cache> `
  -Dfoundry.test.wav=<absolute-wav-file>
```

The test reuses one loaded model for repeated PCM requests, covers final
results, cancellation, callback failures, deterministic cleanup, manager
recreation, and verifies that no callback or worker survives close.
