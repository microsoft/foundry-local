# Migration Plan: C# Changes Since C++ Port Branch

> Covers all commits to `dev/FoundryLocalCore/main` after the C++ port was branched
> (base commit `611cb028`, March 10, 2025). Each commit is assessed for applicability
> to the C++ SDK, with migration scope and priority.

Migration was done of the C# code as of April 10. 
dev/FoundryLocalCore/main Commit ID: 5527c9e4b2b608b72792bb26da0d3e84195ae8bf
https://dev.azure.com/microsoft/windows.ai.toolkit/_git/neutron-server?version=GC5527c9e4b2b608b72792bb26da0d3e84195ae8bf

## Summary

| Status | Count | Description |
|--------|-------|-------------|
| **Migrate** | 5 | Direct behavioral changes that apply to C++ |
| **Partial / Design Input** | 5 | Relevant concepts; C++ needs partial work or should adopt the design |
| **Not Applicable** | 12 | Android-only, NuGet/packaging, NativeInterop-specific, or already covered |

---

## Commits Requiring Migration

### 1. Add chain-of-thought support in grammars

| | |
|---|---|
| **Commit** | `5527c9e4` — PR #15250650 |
| **Priority** | **High** — functional gap, affects model output quality |
| **C# Files** | `OnnxChatGenerator.cs`, `OpenAIServiceProviderOnnx.cs` |
| **C++ Files** | `src/inferencing/generative/chat/onnx_chat_generator.cc`, model info types |

**What changed in C#:**

1. **Think token detection in special token decoding.** The token-sniffing loop that
   detects tool-call special tokens (`specialToken.Contains("tool_call")`) now also
   detects think/reasoning tokens (`specialToken.Contains("think")`). Both are returned
   as special tokens rather than decoded as normal text.

2. **Grammar guidance for chain-of-thought models.** Previously guidance was only applied
   when `toolCallOnly` (tool output requested, no text output). Now it's also applied
   when `cotTextOnly` — the model `SupportsReasoning` and only text output is requested
   (no tool output). This enables constrained decoding for models like DeepSeek R1 Distilled
   that need grammars to produce correct `<think>` tags.

3. **New model metadata fields.** `OpenAILocalModel` gains `SupportsReasoning`,
   `ReasoningStart`, `ReasoningEnd` — populated from the Azure catalog via
   `GetReasoningInfo()`, mirroring the existing `GetToolCallInfo()` pattern.

**C++ current state:**
- Token sniffing in `OnnxChatGenerator` only checks for `"tool_call"`.
- Grammar guidance guard is `tool_output && !text_output` — no reasoning path.
- No `SupportsReasoning` / `ReasoningStart` / `ReasoningEnd` on model info types.

**Status: ✅ Done.**

- Added `supports_reasoning`, `reasoning_start`, `reasoning_end` to `ModelInfo` and
  `CatalogTags`. Populated from catalog data in `AzureCatalogModels`.
- Extended special token detection in `OnnxChatGenerator` to match `"think"`.
- Added the `cotTextOnly` path to grammar guidance: apply grammar when
  `supports_reasoning && !tool_output && text_output`.
- Propagated reasoning fields through `ToolCallContext`.
- Ported all 15 grammar cases from C# `Grammar.cs` to C++ `grammar.cc/grammar.h`
  with full comment parity.
- Restructured `Decode()` in `onnx_chat_generator.cc`, cached EOS tokens.
- 27+ grammar tests, catalog reasoning field tests, ModelInfo round-trip tests.

---

### 2. Add language token and transcribe token by default

| | |
|---|---|
| **Commit** | `58a9d21e` — PR #15139376 |
| **Priority** | **High** — affects audio transcription accuracy |
| **C# Files** | `AudioClient.cs` |
| **C++ Files** | `src/inferencing/generative/audio/onnx_audio_generator.cc` |

**What changed in C#:**
- When no language is provided or language is unsupported, default to `<|en|>` (English)
  instead of empty string.
- Always include `<|transcribe|>` as a separate token in the decoder prompt, even when
  defaulting. Old code only included it when a valid language was provided.

**Before:** `<|startoftranscript|><|notimestamps|>` (no language → no transcribe token)
**After:** `<|startoftranscript|><|en|><|transcribe|><|notimestamps|>` (always)

**Status: ✅ Done.**

`BuildWhisperPrompt()` now defaults to `"en"` when language is empty or unrecognized.
All paths produce `<|startoftranscript|><|{lang}|><|transcribe|><|notimestamps|>`.

---

### 3. Add qwen3_vl and qwen3_5 to IsMultiModal

| | |
|---|---|
| **Commit** | `fbf95c1c` — PR #15196327 |
| **Priority** | **Medium** — needed when these models are used |
| **C# Files** | `GenAIConfig.cs` |
| **C++ Files** | `src/inferencing/generative/genai_config.cc` |

**What changed in C#:**
- `IsMultiModal()` expanded from `phi3v || whisper || phi4mm` to also include `qwen3_vl`,
  `qwen3_5`, and `fara`.

**Status: ✅ Done.**

`OnnxModel::IsMultiModal()` now includes `qwen3_vl` and `qwen3_5`. Test updated.

---

### 4. WebGPU EP fallback for generic-gpu models

| | |
|---|---|
| **Commit** | `c6b572e0` — PR #15046164 |
| **Priority** | **Medium** — needed on platforms where DML isn't available |
| **C# Files** | `ModelManager.cs` |
| **C++ Files** | `src/inferencing/model_load_manager.cc` |

**What changed in C#:**
- For `"generic-gpu"` models, the EP selection was: if CUDA available → use CUDA.
- Now: if CUDA available → use CUDA; **else if WebGPU available** → use WebGPU (with
  comment explaining DML models are compatible with WebGPU).

**Status: ✅ Done.**

Added `else if (HasEP("WebGpuExecutionProvider"))` fallback after the CUDA check in
`ModelLoadManager`. Generic-gpu models now try CUDA → WebGPU → default.

---

### 5. Download retry until global timeout

| | |
|---|---|
| **Commit** | `d61be4dd` — PR #15184561 |
| **Priority** | **Medium** — robustness improvement for flaky networks |
| **C# Files** | `AzureExtensions.cs` |
| **C++ Files** | `src/download/blob_downloader.cc` |

**What changed in C#:**
- Chunk download retry changed from "fixed 3 attempts" to "retry with exponential backoff
  (2s → 30s cap) until the global cancellation/timeout triggers".
- Non-retryable errors cancel all remaining chunks immediately.

**C++ current state:**
- `BlobDownloader` has no explicit retry logic for failed chunk downloads.

**Status: ✅ Done.**

Refactored to use Azure SDK native features instead of a custom retry loop:
- Configured `BlobClientOptions::Retry` with 10 retries, 2s→30s exponential backoff,
  429 (TooManyRequests) added to retryable status codes.
- Uses `Azure::Core::Context` for cooperative cancellation — interrupts blocked I/O
  on both WinHTTP (Windows) and curl (Linux) transports.
- Cancellation bridged from `std::atomic<bool>*` (set by progress callback returning
  non-zero) to `Context::Cancel()` between batches.
- On batch failure, cancels context to fast-drain remaining in-flight futures.
- Catches `Azure::Core::OperationCancelledException` and translates to
  `FOUNDRY_LOCAL_ERROR_OPERATION_CANCELLED`.
- Tests: orchestration cancellation, cancelled-flag propagation, 16 download tests pass
  including live E2E.

---

## Commits with Partial Relevance (Design Input)

### 6. Explicit EP Download and Register entry points

| | |
|---|---|
| **Commit** | `bd3e462c` — PR #15130594 |
| **Priority** | **Design input** — C++ EP detection is stubbed; adopt this design when implementing |
| **C# Files** | Multiple (17 files, large refactor) |
| **C++ Files** | `src/ep_detection/ep_detector.h/.cc`, `src/inferencing/model_load_manager.cc`, catalog |

**What changed in C#:**
- Replaced automatic EP registration with explicit `GetDiscoverableEps()` +
  `DownloadAndRegisterEpsAsync(names?)` two-step flow.
- Catalog no longer blocks on EP finalization — returns models based on currently
  available EPs. Callers re-fetch after EP registration.
- Added `InvalidateCache()` to `IModelCatalog` — called after EP registration succeeds.
- `ModelManager` now checks EP availability before loading, with a clear error message
  listing required vs available EPs and a table of model-id substrings → expected EPs.
- Removed the legacy `WinAppSDKEpDetector`, replaced with `WinMLEpBootstrapper`.

**C++ current state:**
- `IEpDetector` is stubbed (returns CPU only).
- No EP availability check during model load.
- No catalog invalidation mechanism.

**Status: 🔄 In Progress.**

Full implementation plan in [docs/EpDetectionPlan.md](EpDetectionPlan.md). Summary:
- Replace stub `EpDetector` with real hardware detection via WinML EP catalog C API
  (`WinMLEpCatalog.h` from `Microsoft.Windows.AI.MachineLearning` NuGet, fetched directly
  from nuget.org — WinML 2.x is reg-free, no Windows App SDK bootstrap needed).
- `WinMLEpBootstrapper` wraps the WinML C API for EP enumeration + download + registration.
- `CudaEpBootstrapper` handles manual CUDA EP download from Azure CDN + registration.
- `EpDetector` orchestrator manages bootstrappers, coordinates download, and invalidates
  catalog cache after new EPs register.
- C ABI surface: `GetDiscoverableEps`, `DownloadAndRegisterEps`, `IsEpDownloadInProgress`.
- EP availability guard in `ModelLoadManager` with actionable error messages.

---

### 7. Download UX improvements

| | |
|---|---|
| **Commits** | `7b3114b3`, `67b7d4e8`, `ca16c0ea` — PRs #15068098, #15171866, #15200460 |
| **Priority** | **Design input** — some improvements applicable to C++ |
| **C# Files** | `AzureExtensions.cs`, `Configuration.cs`, `NativeInterop.cs` |
| **C++ Files** | `src/download/blob_downloader.cc`, `src/configuration.h` |

**Cumulative C# improvements:**
- Download files smallest-to-largest for faster initial perceived progress
- Configurable concurrency via `NumModelDownloadThreads` (default 64, Android 8)
- Report 0% progress immediately at download start
- Chunk size minimum reduced from 4MB to 2MB
- Heartbeat during container resolution to prevent UI stalls
- Pre-allocate files via `SetLength` for chunked writes
- Richer progress messages with filename + percent (not just percent)
- Remove progress reporting from processes waiting on another process's download

**Status: ✅ Done.**

Implemented the applicable UX improvements:

1. **Sort blobs smallest-first** — `std::sort` by `content_length` in
   `DownloadBlobsToDirectory` after filtering, before download loop.
2. **Emit 0% at download start** — `DownloadBlobsToDirectory` emits `progress(0.0f)`
   after calculating total size, before the first blob. Cancellation-aware (checks
   return value).
3. **Configurable concurrency** — `DownloadManager` constructor takes
   `int max_concurrency` (default 64). `Manager` reads `"NumModelDownloadThreads"`
   from `Configuration::additional_options`, parses with bounds check (min 1), and
   passes it through.
4. **Heartbeat before container resolution** — `DownloadManager::DownloadModel` emits
   `progress_cb(0.0f)` immediately after creating the signal file, before
   `ResolveModelContainer()`. Caller gets a signal that the download process is alive.

**Not ported (by design):**
- Richer progress (filename + percent) — C ABI signature is `int (*)(float, void*)`;
  no way to pass filename without an ABI-breaking change. No consumer needs it.
- Background heartbeat thread — the 0% emit before resolution is sufficient; a
  periodic timer for a ~1s HTTP call is over-engineering.

---

### 8. Realtime audio streaming infrastructure

| | |
|---|---|
| **Commit** | `338a8c7e` — PR #14951245 |
| **Priority** | **Deferred** — already tracked as TODO; C# provides the design reference |
| **C# Files** | `AudioStreamingSession.cs` (new, 369 lines), `NativeInterop.cs` (+249 lines) |
| **C++ Impact** | `AudioSession`, `ItemQueue` (streaming input) |

**What C# added:**
- `AudioStreamingSessionFactory` — `ConcurrentDictionary`-based lifecycle management
- `AudioStreamingSession` — wraps `StreamingProcessor` + `Generator` + `Tokenizer` pipeline
  for incremental Whisper transcription
- Three command entry points: `audio_stream_start`, `audio_stream_push`, `audio_stream_stop`
- Binary transport via `StreamingRequestBuffer` to avoid base64 overhead

**C++ design alignment:**
This maps directly to the `ItemQueue`-based streaming input design already documented in
`CppPortGuide.md`. The C++ approach is architecturally cleaner:
- `AudioSession::ProcessRequest()` receives a `Request` with an `ItemQueue` input item
- `AudioItem` in the queue signals format, `BytesItem` instances stream PCM chunks
- `ItemQueue_MarkFinished` signals end of input
- No need for separate native command entry points — it's just a session with streaming input

**Action:** ~~No migration needed now.~~ **Done.** Realtime audio streaming is implemented using the C++ `ItemQueue` streaming input pattern as planned. Key deliverables:
- `AudioItem` extended with `sample_rate`/`channels` fields (C ABI + internal)
- `ItemQueue::WaitAndPop()` blocking read with condition variable
- `AudioSession::ProcessStreamingAudio()` — incremental PCM → text pipeline
- PCM s16le → float32 conversion
- C# SDK `LiveAudioTranscriptionSession` wraps native streaming via `ItemQueue` push
- 5 integration tests (streaming chunks, initial data, empty queue, cancellation, streaming callback)

---

### 9. Cancellable downloads via callback + configurable timeout

| | |
|---|---|
| **Commit** | `b4bbd32d` — PR #15066223 |
| **Priority** | **Already handled** in C++ C ABI, but timeout configurability is not |
| **C# Files** | `NativeInterop.cs` |
| **C++ Files** | C ABI streaming callback, `src/download/` |

**What changed in C#:**
- Callback return type changed from `void` to `int` (0=continue, 1=cancel).
- Download progress callback checks return value and cancels via `CancellationTokenSource`.
- Configurable timeout (1-120 min, default 5) via `TimeoutMinutes` param.

**C++ current state:**
- Streaming callback already returns `int` (0=continue, non-zero=cancel). ✅
- Download progress callback supports cancellation via return value. ✅

**Configurable timeout — not needed.** The C# timeout was required because the
NativeInterop command-dispatch layer abstracted away direct caller control — the managed
code needed a way to cancel a blocking native call. In the C++ SDK, the caller owns the
progress callback directly and can return non-zero at any time to cancel. The caller *is*
the timeout policy. No SDK-imposed timeout needed.

---

### 13. ORT runtime library path control

| | |
|---|---|
| **Commit** | `5c68c52d` |
| **Priority** | **Design input** — underlying problem (system ORT DLL conflicts) applies to C++ |
| **C# Files** | `NativeInterop.cs` |
| **C++ Files** | `CMakeLists.txt`, `src/exports.def`, new `src/util/dll_loader_windows.cc` |

**What changed in C#:**
- Checks if ORT libraries are already loaded before attempting reload from disk.
- Prevents `DllNotFoundException` when ORT is pre-loaded by `DllLoader.cs`.

**C++ current state:**
- Load-time linking via import libraries (`.lib`). ORT DLLs must be resolvable
  when `foundry_local.dll` is loaded — *before* any C ABI function can be called.
- Co-location (post-build copy to output dir) is the only isolation mechanism.
- The C# SDK v2 works around this in `DllLoader.cs` by pre-loading ORT DLLs
  with `NativeLibrary.TryLoad()` before loading `foundry_local.dll`. Every other
  language binding would need to independently implement the same pre-load dance.

**Why it matters for C++:**
- On Windows, a system-installed `onnxruntime.dll` in the PATH can win the DLL
  loader race, causing API version mismatches at runtime.
- EP detection (planned) will call `OrtEnv::GetEpDevices()` — a direct
  `onnxruntime.dll` API call — *before* any ORT GenAI usage. This means
  `onnxruntime.dll` must be loaded from the correct path before EP detection runs.
- The load order is: `onnxruntime.dll` first, then `onnxruntime-genai.dll` (which
  has its own load-time import of ORT, resolved via Windows module deduplication).

**Planned approach:** Delay-load linking + C ABI path hook. See
[docs/OrtRuntimeLoading.md](OrtRuntimeLoading.md) for the full design, including
the delay-load hook implementation, load order contract, and C ABI surface.

**Status: ✅ Done. Superseded — see [OrtRuntimeLoading.md](../docs/OrtRuntimeLoading.md).**

The delay-load + `RuntimeLibraryPath` + `EagerLoadOrtDlls` design was implemented
on Windows but never had a portable POSIX equivalent (`DT_NEEDED` is always
eager — there is no Linux/macOS counterpart to MSVC `/DELAYLOAD`). It was
replaced by a binding-owned preload contract: every language binding loads
`onnxruntime` then `onnxruntime-genai` by absolute path before loading
`foundry_local`. The native library now has no ORT-loading machinery; ORT and
GenAI are ordinary load-time link dependencies on every platform. Co-location
remains the zero-configuration default for in-tree builds. See
[OrtRuntimeLoading.md](../docs/OrtRuntimeLoading.md) for the full contract.
- 623 tests pass.

---

## Commits Not Applicable to C++

### 10. Android build errors — `71d2e810`

Wraps `ResponsesApi` and native library resolver in `#if !IS_ANDROID`. The C++ SDK has
its own build system and doesn't use `NativeInterop.cs`. Android cross-compilation for
C++ is a separate build concern handled by CMake toolchains. **Not applicable.**

### 11. CUDA binary URL updates — `234b4313`

Updates download URLs and hashes for CUDA/cudnn binaries in `CudaEpBootstrapper.cs`.
EP bootstrapping is not ported to C++. The C++ SDK assumes EPs are pre-installed or
available through the ORT session. **Not applicable** (until EP bootstrapping is
implemented, at which point binary URLs will be different anyway).

### 12. get_version command in C API — `eb382860`

Adds a `"get_version"` command to the C# `NativeInterop` dispatch. The C++ SDK has
its own version API: `FoundryLocalGetVersionString()` exported from the C ABI.
**Already covered by different mechanism.**

### 14. License update — `5a800d59`

Updates `LICENSE.txt` content. C++ SDK now has its own `LICENSE.txt` (identical MIT).
**Done.**

### 15. Per-EP download progress bars — `a768b6a3`

Wires per-EP progress to native callback FFI. Depends on EP bootstrapping infrastructure
that isn't ported. **Not applicable** until EP detection is implemented.

### 16. Null catalog check in WinML bootstrapper — `044c2c30`

Adds `null` check for `ExecutionProviderCatalog` and OS version guard in
`WinMLEpBootstrapper.cs`. WinML-specific code, not ported. **Not applicable.**

### 17. Consolidate MaxConcurrency / IS_ANDROID — `42a5c4af`

Removes duplicate `MaxConcurrency` default in C# `Configuration.cs`. The consolidation
itself is a C#-only cleanup. The *concept* of configurable concurrency is captured in
item #7 (Download UX improvements). **Not applicable as a standalone change.**

### 18. No progress from waiting process — `ca16c0ea`

Large deletion in `AzureExtensions.cs` removing cross-process download progress reporting.
Simplifies the download flow. Captured as design input in item #7. **Not applicable as
a standalone change.**

### 19. ORT/GenAI version updates + minor bug fixes — `6f15a460`

Updates NuGet package versions, CUDA binary hashes, and changes fallback logger level from
Information to Warning. C++ manages ORT GenAI version via vcpkg. The logger level change is
a minor operational tweak. `AudioClient` changes are a 1-line telemetry extension (adding
memory/CPU/GPU metrics to token perf) — telemetry is stubbed in C++.
**Not applicable** (version management differs; telemetry not implemented).

### 20. WinML ORT version pin — `a80d58c0`

Changes the WinML NuGet package dependency version token. NuGet packaging concern.
**Not applicable.**

### 21. ORT-GenAI 0.13.1 update — `c9157bb3`

Updates NuGet versions and CUDA binary URLs. C++ uses vcpkg for ORT GenAI versioning.
**Not applicable** (but the C++ vcpkg manifest should be updated to use the latest
compatible ORT GenAI version independently).

---

## Recommended Migration Order

Sequence based on priority, dependencies, and risk:

| Order | Item | Effort | Risk |
|-------|------|--------|------|
| ~~1~~ | ~~**Audio language/transcribe token default** (#2)~~ | ~~Small~~ | ✅ Done |
| ~~2~~ | ~~**Add qwen3_vl/qwen3_5 to IsMultiModal** (#3)~~ | ~~Trivial~~ | ✅ Done |
| ~~3~~ | ~~**WebGPU EP fallback** (#4)~~ | ~~Small~~ | ✅ Done |
| ~~4~~ | ~~**Chain-of-thought grammar support** (#1)~~ | ~~Medium~~ | ✅ Done |
| ~~5~~ | ~~**Download retry with backoff** (#5)~~ | ~~Medium~~ | ✅ Done |
| ~~6~~ | ~~**Download UX improvements** (#7)~~ | ~~Medium~~ | ✅ Done |
| ~~7~~ | ~~**ORT delay-load + path hook** (#13)~~ | ~~Medium~~ | ✅ Done |
| 8 | **EP detection design** (#6) | Large (when implementing EP detection) | — |
| 9 | **Realtime audio streaming** (#8) | Large (when implementing) | — |

Items 1-7 are complete. Items 8-9 are deferred features tracked elsewhere —
they require substantial new infrastructure (EP bootstrapping, streaming audio
pipeline) and will be implemented when those features are prioritized.
