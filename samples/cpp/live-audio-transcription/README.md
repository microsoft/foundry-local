# Live Audio Transcription Example (C++)

Demonstrates real-time microphone-to-text using the Foundry Local C++ SDK.

Uses [PortAudio](http://www.portaudio.com/) for cross-platform microphone capture
(the C/C++ equivalent of `naudiodon2` used by the JS sample). If PortAudio is not
available, falls back to synthetic PCM audio.


## Artifact compatibility harness

From `samples/cpp`, use `test-v2.ps1` to validate this sample against one of
three distinct v2 artifact sources. The harness leaves this forward-looking
sample unchanged, so a header or API compatibility failure is reported as a
real compiler failure.

```powershell
# Build the canonical local sdk_v2/cpp artifacts, then compile and link.
pwsh ./test-v2.ps1

# Reuse an existing canonical local build and optionally run with --synth.
pwsh ./test-v2.ps1 -ArtifactSource Local -SkipBuild -Run -TimeoutSec 120

# Restore the exact Runtime package from the public ORT-Nightly feed.
# On Windows this validates and compiles only: the package intentionally has no .lib.
pwsh ./test-v2.ps1 -ArtifactSource NuGet -PackageVersion 2.0.0-rc1

# Download a direct pipeline ZIP and compile/link when it contains headers,
# a platform runtime, and a link library.
pwsh ./test-v2.ps1 -ArtifactSource Zip -Run
```

The script defaults to the `2.0.0-rc1` pipeline artifact URL. Pass `-ZipUrl`
to validate a different build.

Use `-Sample live-audio-transcription` to select this sample explicitly. All
downloads, extracts, package caches, compiler outputs, and run logs are kept
under `sdk_v2/cpp/build/sample-artifacts`.

## Manual build

```bash
# With PortAudio (live microphone)
g++ -std=c++20 -DHAS_PORTAUDIO main.cpp -lfoundry_local -lportaudio -o live-audio-transcription-example

# Without PortAudio (synthetic audio only)
g++ -std=c++20 main.cpp -lfoundry_local -o live-audio-transcription-example
```

## Run

```bash
# Live microphone (requires PortAudio)
./live-audio-transcription-example

# Synthetic 440Hz sine wave (no microphone needed)
./live-audio-transcription-example --synth
```

Press `Ctrl+C` to request a graceful stop. The sample passes that signal to
execution-provider and model downloads so long-running downloads can be
cancelled before transcription starts.
