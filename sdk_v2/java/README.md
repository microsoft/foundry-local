# Foundry Local Java SDK Preview

Java 17+ bindings for in-process model inference through the Foundry Local C
API. Streaming ASR is the first supported inference scenario. The SDK uses JNA,
keeps the native runtime and model weights outside the JAR, and exposes
deterministic `AutoCloseable` lifetimes.

This package is a preview and is not published to Maven Central yet.
Its first phase intentionally covers streaming ASR rather than the full
cross-language SDK surface. Generic Session/Request/Response/Item APIs, Chat,
and native file/URI transcription are future extension points.
The staged Maven coordinates are
`com.microsoft.foundry:foundry-local-sdk:<preview-version>`.

## Build

Prerequisites:

- JDK 17 or newer
- Maven 3.9 or newer

```powershell
mvn -f sdk_v2/java/pom.xml package
```

The build produces:

- `target/foundry-local-sdk-0.1.0-preview.2-SNAPSHOT.jar`
- `target/foundry-local-sdk-0.1.0-preview.2-SNAPSHOT-sources.jar`

`verify` also runs SpotBugs and fails on actionable findings. The packaging
pipeline compiles a clean Maven consumer against the staged POM and JAR.

Override `revision` when producing an immutable release:

```powershell
mvn -f sdk_v2/java/pom.xml -Drevision=0.1.0-preview.2 package
```

JNA remains a normal Maven dependency. The SDK JAR does not contain JNA native
code, Foundry Local native libraries, execution providers, or model weights.

## Runtime setup

Prepare a directory containing the matching Foundry Local native runtime for
the current OS and architecture. Pass that directory through `Configuration`.
The Java binding requires C API v2. Runtimes exposing only v1 are rejected
before their native function tables are accessed.

The runtime directory must contain:

- Windows: `foundry_local.dll`
- Linux: `libfoundry_local.so`
- macOS: `libfoundry_local.dylib`

When ONNX Runtime or ONNX Runtime GenAI libraries are present in the same
directory, the SDK preloads them before Foundry Local. Otherwise, the platform
loader must be able to resolve those dependencies.

The first successfully resolved runtime directory remains loaded for the JVM
lifetime. After closing a manager, another manager can be created only with
the same resolved runtime directory. Start a new JVM to use a different native
runtime directory.

The platform-independent JAR can run wherever the matching Foundry Local native
runtime is available. Current upstream native artifacts target Windows x64 and
ARM64, Linux x64 and ARM64, and macOS ARM64. Use a 64-bit JVM with the same
architecture as the native runtime.

On Java 25, pass `--enable-native-access=ALL-UNNAMED` when required by the JVM.
The older JBR 21.0.8 and 21.0.9 builds tested with Runtime 2.0.1 on Windows load
an older C runtime first and cannot initialize ONNX Runtime. Use a compatible
JBR/native-runtime combination instead of replacing IDE or system DLLs.

The manager defaults native logging to Fatal and disables nonessential
telemetry. Applications can override either setting through the
`Configuration` builder, for example with `logLevel(LogLevel.DEBUG)` and
`disableNonessentialTelemetry(false)`. Runtime diagnostics such as
`ORTGENAI_ORT_VERBOSE_LOGGING` can also control the underlying ONNX Runtime
GenAI logging where supported.

## Streaming ASR

The model is loaded once and reused across successive dictation requests.
Each `AudioSession` accepts one active `Transcription`; close it before starting
the next request.

```java
var configuration = Configuration.builder(
        "my-app")
        .runtimeDirectory(Path.of("native-runtime"))
        .modelCacheDirectory(Path.of("model-cache"))
        .appDataDirectory(Path.of("app-data"))
        .build();

try (var manager = new FoundryLocalManager(configuration)) {
    var model = manager.catalog()
            .getModelVariant("nemotron-speech-streaming-en-0.6b-generic-cpu:3");

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

`Catalog.getModel(alias)` queries an alias; `getModelVariant(name:version)`
queries an exact variant and throws `ModelNotFoundException` when unavailable.
`models()` returns alias-level entries, while `modelVariants()` returns all
individual variants. `manager.catalog()` is the public catalog shortcut.
Use `manager.catalog(CatalogType.LOCAL)` for cached or caller-registered local
models and future bring-your-own-model scenarios.

`Model.info()` returns a detached immutable snapshot that remains usable after
the manager closes. Named accessors cover common metadata, while
`stringProperties()`, `intProperties()`, and `modelSettings()` preserve
forward-compatible key/value shapes without growing a positional constructor.
The current native API can query arbitrary model property keys but cannot
enumerate them, so the property maps contain the well-known keys supported by
this SDK version.

This preview uses Foundry Local's native streaming-audio processor. The ASR
task in catalog metadata is necessary but does not promise that every
file-oriented ASR model supports this path. Use a model whose native runtime
supports streaming audio; the example and integration test use the Nemotron
streaming model. `transcribeWav` decodes a supported PCM WAV file in Java and
feeds its samples through the same streaming path. It does not invoke native
file/URI transcription, and this preview does not promise file-input parity
for models such as Whisper.

For a real microphone stream, create one `Transcription`, call `writePcm` for
each chunk, then call `finishInput` and `await`. `finishInput` drains queued
audio and publishes the final transcript. `cancel` requests native cancellation
and produces a result with `cancelled() == true`.

PCM input is intentionally limited to signed PCM16LE, 16 kHz, mono. Each call
accepts at most one second of audio, and the SDK applies backpressure after two
seconds are queued.

## Ownership and threading

- `FoundryLocalManager` owns catalogs, models, and sessions. Only one manager
  may be open at a time. After close, a manager can be recreated in the same
  JVM only with the same resolved runtime directory.
- A loaded `Model` can create multiple successive `AudioSession` instances
  without reloading the model.
- Closing a manager closes outstanding sessions and transcriptions before
  releasing the native manager.
- If the JVM exits while a manager is still open, a shutdown hook closes it so
  the native runtime is released before its static destructors run. Do not
  call `System.exit` from SDK callbacks, because the hook waits for callbacks.
- Download progress and speech listeners run on native callback threads. Keep
  callbacks short and do not call SDK lifecycle or input methods from them.
- `Transcription.close()` cancels unfinished work and waits for native callbacks
  before releasing request and PCM buffers.

## Tests

```powershell
mvn -f sdk_v2/java/pom.xml test
```

The native ASR integration test is opt-in and never downloads a model:

```powershell
mvn -f sdk_v2/java/pom.xml test `
  -Dtest=NativeAsrTest `
  -Dfoundry.test.runtime=<absolute-native-runtime-dir> `
  -Dfoundry.test.cache=<absolute-model-cache> `
  -Dfoundry.test.wav=<absolute-wav-file> `
  -Dfoundry.test.model=nemotron-speech-streaming-en-0.6b-generic-cpu:3
```

CI provides the equivalent `FOUNDRY_LOCAL_NATIVE_BIN_DIR`,
`FOUNDRY_TEST_DATA_DIR`, and `FOUNDRY_TEST_WAV` environment variables and
`FOUNDRY_TEST_MODEL`, and passes `-Dfoundry.test.native.required=true`, so
missing native inputs fail instead of skipping the integration test.

The test reuses one loaded model for repeated PCM requests, covers final
results, cancellation, callback failures, deterministic cleanup, manager
recreation with the same runtime directory, and verifies that no callback or
worker survives close. It also launches a child JVM to check that an abandoned
stream neither blocks nor crashes process exit. The Windows x64 CI lane
separately probes a checksum-pinned released legacy runtime in a fresh JVM to
verify rejection before accessing incompatible native function tables.
