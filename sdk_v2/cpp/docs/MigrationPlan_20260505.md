# Migration Plan: Changes Since April 10 Sync

> Covers all commits to `neutron-server` (`dev/FoundryLocalCore/main`) and
> `Foundry-Local` (`main`) after April 10, 2026. Each change is assessed for
> applicability to the C++ SDK core and the internal C# SDK v2 wrapper.

## Summary

| Status | Count | Description |
|--------|-------|-------------|
| **Migrate (Core)** | 3 | New features needed in the C++ core |
| **Migrate (SDK)** | 3 | SDK-level API changes (C# SDK v2, public C++ SDK) |
| **Partial / Design Input** | 2 | Concepts to adopt partially |
| **Not Applicable** | ~15 | JS/Rust/Python only, packaging, CI, already handled |

### Implementation Status (as of 2026-05-07)

| # | Item | Status |
|---|------|--------|
| 1 | Embedding support — core (C++ session, web endpoint, batched) | ✅ Done |
| 2 | Download region parameter | ✅ Done |
| 3 | WebGPU EP bootstrapper | ✅ Done |
| 4 | Embedding API to C++ SDK / C ABI surface | ✅ Done |
| 5 | Rename `GetTranscriptionStream` → `GetStream` (C# SDK v2) | ✅ Done |
| 6 | Public C++ SDK callback return type fix | N/A (already correct internally) |
| 7 | Telemetry simplification | ⏳ Deferred (waits for real telemetry impl) |
| 8 | LiveAudio `id` field | ✅ Done |
| 9 | WebGPU EP per-platform hashes + URL bump | ✅ Done |
| 10 | ORT runtime / EP version logging | ✅ Done |
| 11 | OpenAI streaming usage chunk + error envelope | ✅ Already-equivalent |

### Previous Plan Status

All 5 "Migrate" items from [MigrationPlan.md](MigrationPlan.md) are **done**:
- ✅ Chain-of-thought grammar support (#1)
- ✅ Audio language/transcribe token default (#2)
- ✅ qwen3_vl/qwen3_5 multimodal (#3)
- ✅ WebGPU EP fallback (#4)
- ✅ Download retry with backoff (#5)

---

## Commits Requiring Migration (Core — C++ Native Library)

### 1. Add embedding support to Foundry Local Core

| | |
|---|---|
| **Source** | neutron.main `85ee0410` + `352a76b0` — PRs #15212502, #15374879 |
| **Date** | 2026-04-15, 2026-04-21 |
| **Priority** | **High** — new feature, already exposed in all SDKs |
| **C# Files** | `OnnxEmbeddingsGenerator.cs`, `EmbeddingsClient.cs`, `GenAIConfig.cs`, `OpenAIApiProvider.cs`, `WebApplicationFactory.cs` |

**What changed in C#:**

1. **`OnnxEmbeddingsGenerator`** — New class. Takes tokenized input, runs one forward
   pass (`GenerateNextToken()`), extracts `hidden_states` output from the generator,
   does last-token pooling (takes last `hidden_size` elements), and L2-normalizes
   the result to produce a float embedding vector.

2. **`ApplyEmbeddingTemplate`** — Encodes input text, appends EOS token ID. Embedding
   models require EOS because they use the last token's hidden state as the sentence
   embedding (last-token pooling strategy).

3. **`GenAIConfig`** — Added `hidden_size` field (read from `genai_config.json`).
   Used as authoritative embedding dimension; falls back to inferring from output
   length if not present.

4. **Batch support** — Shared `Embeddings` handler loops over all inputs, creates one
   generator per input string, collects `EmbeddingData` entries with index.

5. **FP16 fix (352a76b0)** — `Generator.GetOutput("hidden_states")` returns FP16 for
   quantized models. Detects tensor element type via `Tensor.Type()` and converts
   FP16→FP32 using `BitConverter.UInt16BitsToHalf`. Also limits `max_length` on
   `GeneratorParams` to `totalTokenCount + 1` to avoid OOM from oversized KV cache.

6. **Web endpoint** — `POST /v1/embeddings` registered in the web service. Accepts
   `{ "model": "...", "input": "text" | ["text1", "text2"] }`. Returns OpenAI-
   compatible `EmbeddingCreateResponse`.

**C++ current state:**
- No embedding support whatsoever — no generator, no endpoint, no session type.
- `GenAIConfig` does not read `hidden_size`.

**Migration work:**

1. Add `hidden_size` field to `GenAIConfig` (read from `genai_config.json`).
2. Add `SessionType::kEmbeddings` to `inferencing/session/types.h`.
3. Create `EmbeddingsSession : Session` following the same pattern as `ChatSession` /
   `AudioSession`:
   - Created cheaply by `Session::Create()` factory based on the model's task type.
     Model stays loaded in `ModelLoadManager` across session creation/destruction.
   - Constructed with `allow_concurrent_requests = true` — each embedding request is
     fully stateless (no conversation history, no shared generator state), so
     requests can run in parallel against the same loaded model.
   - `ProcessRequestImpl(request, response)`:
     - Pull input text(s) from `request` items (`TextItem` or array thereof).
     - For each input string, create a fresh `OgaGenerator` + `GeneratorParams`
       (cheap — model is already loaded), run `ApplyEmbeddingTemplate` (encode +
       append EOS), set `max_length = token_count + 1` to bound KV cache, run one
       `GenerateNextToken()`, extract `hidden_states`, handle FP16→FP32 via tensor
       element type, last-token pool, L2-normalize.
     - Push one `TensorItem` (1-D float vector) per input into `response`.
4. Add `POST /v1/embeddings` handler to the web service. Handler:
   - Deserializes `EmbeddingCreateRequest`, builds a `Request` with `TextItem`s,
     calls `EmbeddingsSession::ProcessRequest`, serializes `TensorItem` outputs
     into `EmbeddingCreateResponse`.
5. Define contract types in `src/contracts/embeddings.h` (+ JSON converters).
   Support both single string and array input on `"input"`.
6. Expose through C ABI: `Session::Create()` already returns the right session type
   based on model metadata. Verify the C ABI session creation path works for
   embedding models without new entry points. Add a `MODEL_PROP_TASK_TYPE` value
   for embeddings if not present.

**Effort:** Large (new feature end-to-end). ~300-400 lines of new code.
**Risk:** Medium — straightforward port but touches model loading, session, web service.

#### ✅ Status: Done (2026-05-05)

**Files added:**
- `src/inferencing/generative/embeddings/embeddings_session.h/.cc` — `EmbeddingsSession`
- `src/contracts/embeddings.h/.cc` — `EmbeddingCreateRequest` / `EmbeddingCreateResponse` with ADL JSON
- `src/service/embeddings_handler.h/.cc` — `POST /v1/embeddings` handler

**Files edited:**
- `src/inferencing/session/types.h` — added `SessionType::kEmbeddings`
- `src/inferencing/generative/genai_config.h/.cc` — added `std::optional<int> hidden_size`
- `src/inferencing/session/session.cc` — factory branch for `task == "text-embedding"`
- `src/service/web_service.cc` — registered `/v1/embeddings` route
- `src/telemetry/telemetry.h` — added `Action::kOpenAIEmbeddings`
- `CMakeLists.txt` — added the three new `.cc` files

**Deviations from the original plan (intentional improvements):**

1. **Batched inference (not in the original C# port).** Instead of looping over inputs and
   creating a generator per string (matching C#'s naive loop), the implementation runs a
   *single* batched forward pass for all inputs in one request:
   - Each input becomes a row of one `OgaSequences` (each row gets EOS appended).
   - `GeneratorParams.batch_size = N`, `max_length = max(token_counts) + 1`.
   - GenAI right-pads shorter rows internally with `pad_token_id`.
   - Per-row hidden state extracted at offset `(token_counts[i] - 1) * hidden_size` within
     row `i`. Right-padding is safe under causal attention — the EOS at that position only
     attends to real tokens, so the embedding is bit-identical to a non-batched run.
   - This eliminates per-call overhead (tokenizer setup, generator construction, KV cache
     allocation, kernel launch) for all but the first input — meaningful win for RAG
     indexing workloads with many docs per request.

2. **Validated tensor layout.** Explicit `expected_elements == total_elements` check on the
   `hidden_states` shape so a future GenAI layout change fails loudly rather than silently
   returning wrong embeddings.

3. **FP16 helper extracted.** `Fp16ToFp32` lives as a file-scope static helper (was
   previously duplicated). Handles subnormals, Inf, and NaN per IEEE 754.

4. **Stateless concurrent session.** `EmbeddingsSession` is constructed with
   `allow_concurrent_requests = true` — confirmed by user as the right model since each
   request is fully independent (no KV cache sharing).

---

### 2. Add download region parameter

| | |
|---|---|
| **Source** | neutron.main `f02d5151` — PR #15211761 |
| **Date** | 2026-04-16 |
| **Priority** | **Medium** — needed for non-US deployments |
| **C# Files** | `AzureExtensions.cs`, `Configuration.cs`, `FoundryLocalCore.cs` |

**What changed in C#:**
- `Configuration` gains `ModelRegistryRegion` property (default `"centralus"`), read from
  config key `"CatalogRegion"`.
- `AzureExtensions.ModelRegistryRegion` is set from configuration during init.
- The hardcoded `https://eastus.api.azureml.ms/modelregistry/...` URL becomes
  `https://{ModelRegistryRegion}.api.azureml.ms/modelregistry/...`.

**C++ current state:**
- `model_registry_client.cc` has `https://eastus.api.azureml.ms/...` hardcoded.
- `Configuration` has no region field.

**Migration work:**
- Add `catalog_region` field to `Configuration` (default `"centralus"`).
- Expose via C ABI configuration setter.
- `ModelRegistryClient` uses `configuration.catalog_region` to construct the URL.
- ~15 lines across 3 files.

**Effort:** Small.
**Risk:** Low.

#### ✅ Status: Done

- C ABI: `FoundryLocalSetCatalogRegion` added to `flConfiguration` vtable.
- C++ wrapper: `Configuration::SetCatalogRegion(std::string)`.
- C# wrapper: `Configuration.CatalogRegion` property exposed via P/Invoke.
- `ModelRegistryClient` and `DownloadManager` constructors accept the region; default values added so existing test callsites compile unchanged.

---

### 3. WebGPU EP bootstrapper

| | |
|---|---|
| **Source** | neutron.main `e4c0c07c` — PR #15166865 |
| **Date** | 2026-05-01 |
| **Priority** | **Medium** — needed on machines with WebGPU but no DML |
| **C# Files** | `WebGpuEpBootstrapper.cs` (222 lines, new) |

**What changed in C#:**
- New `WebGpuEpBootstrapper` class (same pattern as `CudaEpBootstrapper`):
  - Downloads WebGPU EP zip from CDN URL
  - Extracts to `{epDir}/webgpu-ep/` directory
  - Verifies binary hashes (`onnxruntime_providers_webgpu.dll`)
  - Uses cross-process file lock to prevent concurrent installs
  - Calls `SetDllDirectory` for proper DLL loading
  - Registers with ORT via `OrtEnvHelper`
  - Retries up to 5 attempts
  - Tracks telemetry via `EPDownloadTracker`

**C++ current state:**
- `CudaEpBootstrapper` exists and follows same pattern.
- `IEpBootstrapper` interface is defined.
- `EpDetector` has infrastructure for multiple bootstrappers.
- No `WebGpuEpBootstrapper`.

**Migration work:**
- Create `webgpu_ep_bootstrapper.h/.cc` following `CudaEpBootstrapper` pattern.
- Same download/extract/verify/lock/register flow.
- Expected binaries: `onnxruntime_providers_webgpu.dll` (+ hash).
- Register with `EpDetector` during initialization.
- ~150-200 lines (reusing existing infrastructure).

**Effort:** Medium (mostly boilerplate following the CUDA bootstrapper).
**Risk:** Low — pattern is well-established.

#### ✅ Status: Done

- New `src/ep_detection/webgpu_ep_bootstrapper.h/.cc` mirrors `CudaEpBootstrapper`.
- Platform-aware CDN URL/binary selection (`win-x64`, `win-arm64`, `macos-arm64`, `linux-x64`).
- SHA256 verification, cross-process file lock, retries, telemetry.
- Registered with the EP manager during initialization.
- **2026-05-07** — picked up upstream `90bcaf10`: per-platform hash constants and
  CDN URL bump to `webgpu_ep_20260504-224804_*.zip`. Earlier single hash matched
  none of the actual binaries; tests now register WebGPU cleanly.

---

### 9. ORT runtime / EP version logging

| | |
|---|---|
| **Source** | neutron.main `0ae26958` — PR #15430283 |
| **Date** | 2026-05-04 |
| **Priority** | **Low** — observability only, no behavior change |
| **C# Files** | `RuntimeVersionInfo.cs` (new), `EpDetector.cs`, `CudaEpBootstrapper.cs`, `WinMLEpBootstrapper.cs`, `OpenAIServiceProviderBase.cs` |

**What changed in C#:** new `RuntimeVersionInfo` helper logs ORT/GenAI versions
at startup and queries `OrtEnv.GetEpDevices()` for per-EP `version` metadata.
Inference paths in `OpenAIServiceProviderBase` wrap calls in try/catch + log
stack trace + rethrow.

**C++ port:**
- New `src/ep_detection/runtime_version_info.h/.cc`:
  - `LogRuntimeVersions(ILogger&)` — reads `OrtApiBase::GetVersionString()`.
    GenAI runtime version is omitted (no public C API for it); SDK build version
    is already on the public API via `foundry_local::Version()`.
  - `GetEpVersion(OrtApi&, OrtEnv&, name)` — walks `GetEpDevices()` and returns
    `EpDevice_EpMetadata["version"]` for the matching EP.
- `manager.cc` calls `LogRuntimeVersions` once at startup; the `register_ep`
  lambda now logs `library=<path>, version=<v>` after each successful
  `RegisterExecutionProviderLibrary`.
- `cuda_ep_bootstrapper.cc` and `webgpu_ep_bootstrapper.cc` log the install
  directory (`install_path=<dir>`) on success — the central callback only sees
  the library path. WinML's library path equals its install location, so no
  extra bootstrapper line is needed.
- Stack-trace capture across the C ABI boundary is handled by `c_api.cc`'s
  `HandleException` already; an explicit per-endpoint try/catch+log mirror is
  not added because exceptions are converted to `flStatus` at the boundary,
  preserving `what()`.

#### ✅ Status: Done (2026-05-07)

---

### 10. OpenAI streaming usage chunk + error envelope

| | |
|---|---|
| **Source** | neutron.main `af3bda19` — PR #15264033 |
| **Date** | 2026-05-04 |
| **Priority** | **Low** — OpenAI .NET SDK compatibility |
| **C# Files** | `ChatCompletions.cs`, `OpenAIUtils.cs`, `OpenAIError*.cs`, `OpenAIApi.cs` |

**What changed in C#:** streaming chat completions now emit a final
`chat.completion.chunk` with empty `choices` and populated `usage`. HTTP errors
use OpenAI-style `{ "error": { message, type, code } }` JSON.

**C++ state:**
- `chat_completions_handler.cc::HandleStreaming` already emits a usage-only
  `ChatCompletionChunk` before `data: [DONE]` when `stream_options.include_usage`
  is true. Matches the OpenAI spec; the C# change made it unconditional, but
  the SDK-level OpenAI client only inspects `usage` when present, so both
  forms are compatible.
- The streaming error path already serializes `{ "error": { message, type,
  param, code } }` (lines 247-250).

#### ✅ Status: Already-equivalent (no code change needed)

---

## Commits Requiring Migration (SDK Layer)

### 4. Add Embedding API to C++ SDK

| | |
|---|---|
| **Source** | fl.sdk `03fef37` — PR #639 |
| **Date** | 2026-04-22 |
| **Priority** | **High** — paired with core embedding support (#1) |
| **C# SDK Files** | `EmbeddingClient.cs`, `EmbeddingRequestResponseTypes.cs`, `IModel.cs` |
| **Public C++ SDK** | Not yet added |

**What changed in external SDKs (C#, JS, Python, Rust):**
- `OpenAIEmbeddingClient` with `GenerateEmbedding(string)` and
  `GenerateEmbeddings(IEnumerable<string>)`.
- `IModel.GetEmbeddingClientAsync()` factory method.
- Calls the native core's `embeddings` command.

**C++ SDK v2 (internal) current state:**
- No embedding client class.
- C ABI has no embedding entry point.

**Migration work:**
- Depends on core embedding support (#1) being implemented first.
- Add `EmbeddingSession` or extend C ABI with embedding entry point.
- Add `OpenAIEmbeddingClient` to C# SDK v2 that wraps the C ABI.
- Add embedding client to public C++ SDK (`sdk/cpp` in fl.sdk).

**Effort:** Medium (once core support exists, SDK layer is thin).

#### ✅ Status: Done (2026-05-05)

No new C ABI surface needed — embeddings are a regular `Session` over the existing C ABI.

**C++ public SDK** (`sdk_v2/cpp/include/foundry_local/`):
- `foundry_local_cpp.h`: added `class EmbeddingsSession : public Session` with task validation and two `Embed()` overloads (single-string, batch).
- `foundry_local_cpp.inline.h`: inline definitions for the `Embed()` helpers — build a `Request` of `TextItem`s, call inherited `ProcessRequest`, validate each response item is a `FOUNDRY_LOCAL_TENSOR_FLOAT` tensor, and copy `shape[0]` floats into the output. Throws `std::runtime_error` on count mismatch, non-tensor item, non-float dtype, or empty shape.

**C# SDK v2** (`sdk_v2/cs/src/`):
- `EmbeddingsSession.cs` (new): sealed `Session` subclass that validates `model.Info.Task == "text-embedding"`.
- `OpenAI/EmbeddingsClient.cs` (new): `OpenAIEmbeddingsClient` mirroring `OpenAIAudioClient`; exposes `GenerateEmbeddingAsync(string)` and `GenerateEmbeddingsAsync(IEnumerable<string>)` returning `float[]` and `IReadOnlyList<float[]>` respectively. Single-input overload delegates to the batch path with one element so we always exploit the C++ core's batched-forward-pass.
- `IModel.cs`: added `Task<OpenAIEmbeddingsClient> GetEmbeddingsClientAsync(CancellationToken?)`.
- `Detail/Model.cs`: factory implementation mirroring `GetAudioClientAsync` (load check + native handle).

**Incidental fix:** `ChatSession.cs` line 21 had a malformed `<see cref="Request"` (missing closing `/>`) producing 29 XML-doc errors that were blocking the C# build. Fixed by closing the tag properly.

---

### 5. Synchronize real-time audio API names

| | |
|---|---|
| **Source** | fl.sdk `b379679` — PR #692 |
| **Date** | 2026-05-04 |
| **Priority** | **Low** — naming consistency only |
| **Affected** | C# SDK v2, public C++ SDK |

**What changed:**
- All SDKs renamed `GetTranscriptionStream` → `GetStream`
- C++ SDK files renamed: `openai_live_audio_client.*` → `openai_live_audio_session.*`

**Internal C# SDK v2 current state:**
- Still uses `GetTranscriptionStream` method name.
- File is `LiveAudioTranscriptionClient.cs`.

**Migration work:**
- Rename `GetTranscriptionStream` → `GetStream` in `LiveAudioTranscriptionClient.cs`.
- Optionally rename file to `LiveAudioSession.cs` for consistency.
- Update any references in tests/samples.

**Effort:** Trivial (~5 minutes).
**Risk:** None — purely cosmetic rename.

#### ✅ Status: Done

- `GetTranscriptionStream` → `GetStream` in `LiveAudioTranscriptionClient.cs`.
- Tests and README updated.
- File rename to `LiveAudioSession.cs` deferred (not required for parity).

---

### 6. Fix download progress callback return type (public C++ SDK)

| | |
|---|---|
| **Source** | fl.sdk `598fc19` — PR #693 |
| **Date** | 2026-05-04 |
| **Priority** | **N/A for internal** — this is a bug fix in the public fl.sdk C++ SDK |

**What changed:**
- Public C++ SDK `NativeCallbackFn` was `void(*)()`, should be `int(*)()` (return 0
  to continue). The `void` return caused garbage in the return register, which the
  native core interpreted as "cancel".

**Internal C++ SDK state:**
- The C ABI streaming callback already returns `int`. ✅
- The internal C# SDK correctly uses `int` return. ✅
- This bug was only in the public fl.sdk C++ thin wrapper.

**Action:** No internal work needed. Already correct in our implementation.

---

## Commits with Partial Relevance (Design Input)

### 7. Simplify telemetry

| | |
|---|---|
| **Source** | neutron.main `a4c019db` — PR #15328995 |
| **Date** | 2026-04-20 |
| **Priority** | **Design input** — apply when implementing real telemetry |

**What changed in C#:**
- Removed `TotalTimeMs` and `ResumedFromState` from download telemetry.
- Added `TelemetryEnvironment` utility that detects CI environments (checks
  `TF_BUILD`, `GITHUB_ACTIONS`, `GITLAB_CI`, etc.) and suppresses telemetry.
- Simplified device tracking (removed `GetDisplayName`, `GetUserLocale`,
  `GetUserTimezone`).

**C++ current state:**
- Telemetry is stubbed (`ITelemetry` interface with placeholder impl).

**What to adopt when implementing real telemetry:**
1. CI environment detection — suppress telemetry when common CI env vars are set.
2. Keep `RecordDownload` signature lean (no total time / resumed state fields).
3. Minimal device tracking — no locale/timezone collection.

---

### 8. Nemotron-ASR / LiveAudio `id` field

| | |
|---|---|
| **Source** | fl.sdk `3334687`, `7e9043f` — PRs #687, #689 |
| **Date** | 2026-05-01 |
| **Priority** | **Low** — parity with public SDK |

**What changed:**
- `LiveAudioTranscriptionResponse` gains an optional `id` field across all SDKs.
- Parsed from native Core's JSON response when present.

**C++ core current state:**
- The internal `AudioSession` streaming response already produces a JSON structure.
- Need to verify if it includes an `id` field. If not, add it.

**Internal C# SDK v2 state:**
- `LiveAudioTranscriptionResponse` — check if `Id` field exists.

**Migration work:**
- Verify the native core produces `id` in streaming audio responses.
- If missing from C# SDK v2 types, add the optional field.
- Small verification task.

#### ✅ Status: Done

- C++ core: added `std::string id` to `AudioTranscriptionResponse`; `to_json` emits it when non-empty.
- Web-service path (`ProcessAudioTranscriptionJson`): emits a `JsonItem` containing the `id` (so REST clients see it round-tripped through JSON).
- C ABI streaming path (`ProcessStreamingAudio`): emits `TextItem`s directly — the C# layer no longer round-trips through JSON for the simple text callback case. **Architectural correction during implementation:** initial impl wrapped streaming text in JSON; user pointed out the direct-text path is more appropriate for the C ABI streaming callback.
- Both paths generate a unique `response_id` via `ResponseConverter::GenerateId("audio")`.
- C# `LiveAudioTranscriptionRaw.Id` field added; `LiveAudioTranscriptionClient` callback handles `TextItem` directly (wraps into the response without JSON parsing) and falls back to `FromJson` only for `JsonItem`.

---

## Commits Not Applicable to Internal C++ SDK

| Commit | Reason |
|--------|--------|
| `c3ab6402` — macOS linker fixes for Core | C++ has its own build system; macOS builds already work |
| `c9157bb3` — ORT GenAI 0.13.1 update | C++ manages via vcpkg independently |
| `a80d58c0` — WinML ORT version pin | NuGet packaging concern |
| `6f15a460` — Small bug fixes + ORT versions | C#-specific dependency updates |
| `ca16c0ea` — No progress from waiting process | Already handled in previous migration |
| `42a5c4af` — Consolidate MaxConcurrency | C#-only cleanup |
| `a768b6a3` — Per-EP download progress bars | Depends on EP bootstrapping infrastructure (design input item) |
| `b4bbd32d` — Cancellable downloads callback | Already correct in C++ C ABI |
| `5a800d59` — License update | Separate license management |
| `eb382860` — get_version command | C++ has `FoundryLocalGetVersionString()` |
| `5c68c52d` — ORT/GenAI path config | C++ links natively, no runtime resolution |
| `1a59b7b` — JS SDK non-blocking init | JS SDK specific |
| `58bba93` — Linux OpenSSL preload | Node.js specific workaround |
| `3b3614f` — JS Node-API addon | JS SDK specific |
| `857aa22` — netstandard2.0 support | C# NuGet packaging |
| `ec94cba` — ORT-Nightly fallback NuGet source | NuGet specific |
| `413925d` — Avoid standard install during WinML | JS install script specific |
| `897d3e7` — Bump ORT versions (fl.sdk) | Per-SDK versioning |
| `984892e` — Nemotron-ASR to public C++ SDK | Public SDK (fl.sdk) — our internal version already has realtime audio |
| `c94bb03`, `99b091f` — Nemotron-ASR to Python/Rust | Other language SDKs |
| `573dbde` — Nemotron samples | Samples only |
| `088f844` — Bump dev version | Packaging |
| Various CI/pipeline commits | CI-specific |

---

## Recommended Migration Order

Sequence based on dependencies, priority, and risk:

| Order | Item | Effort | Depends On | Risk | Status |
|-------|------|--------|-----------|------|--------|
| 1 | **Download region parameter** (#2) | Small (~15 lines) | Nothing | Low | ✅ Done |
| 2 | **Rename `GetTranscriptionStream` → `GetStream`** (#5) | Trivial | Nothing | None | ✅ Done |
| 3 | **Embedding support — core** (#1) | Large (new feature) | Nothing | Medium | ✅ Done |
| 4 | **Embedding support — SDK layer** (#4) | Medium | #3 | Low | ✅ Done |
| 5 | **WebGPU EP bootstrapper** (#3) | Medium (~200 lines) | Nothing | Low | ✅ Done |
| 6 | **LiveAudio `id` field** (#8) | Small (verification) | Nothing | None | ✅ Done |
| 7 | **Telemetry simplification** (#7) | Deferred | Real telemetry impl | — | ⏳ Deferred |

### Pre-existing build fixes (incidental)

While working through the items above, several pre-existing build breaks on this branch were fixed:
- Duplicate CMakeLists targets for `audio_session.cc`.
- Duplicated declarations in `chat_session.h`.
- Unused `session_manager_` reference field in `Session` base.
- Missing includes in test files.
- Warning-as-error from a dead local variable.
- Malformed `<see cref="Request"` XML doc tag in `ChatSession.cs` (29 errors blocking the C# build).

All targets (`foundry_local.dll`, `foundry_local_tests.exe`, `sdk_integration_tests.exe`) now build cleanly.

### Test coverage added

| Item | Tests added | Location |
|------|-------------|----------|
| FP16 conversion (extracted to `fp16.h`) | 5 unit tests | `sdk_v2/cpp/test/internal_api/embeddings/fp16_test.cc` |
| Embeddings JSON contracts | 9 unit tests | `sdk_v2/cpp/test/internal_api/embeddings/contracts_embeddings_test.cc` |
| `GenAIConfig.hidden_size` | 3 unit tests | `sdk_v2/cpp/test/internal_api/genai_config_test.cc` (extended) |
| Region URL construction | 2 unit tests | `sdk_v2/cpp/test/internal_api/download_test.cc` (extended) |
| AudioTranscriptionResponse JSON `id` | 2 unit tests | `sdk_v2/cpp/test/internal_api/audio/audio_transcription_contract_test.cc` |
| LiveAudioTranscription `Id` (C#) | 2 unit tests | `sdk_v2/cs/test/FoundryLocal.Tests/LiveAudioTranscriptionTests.cs` (extended) |

C++ totals: **21 new unit tests, all passing.**
C# totals: 2 new unit tests written and compile cleanly. **Cannot execute due to a pre-existing native interop crash** in `Marshal.PtrToStructure[FlConfigurationApi]` invoked during the test assembly's `[Before(Assembly)]` hook. This crash reproduces against the unmodified branch (verified by stashing all our changes, rebuilding, and re-running) and blocks every test in the C# assembly. **Out of scope for this migration; needs a separate investigation by `@CSharpCoder`.**

### Test gaps still outstanding

These were called out by the test audit but are deferred to a follow-up:

- **Embeddings end-to-end integration test** — requires adding a small embedding model to `SharedTestEnv` and writing a test that asserts `batch_embed([a,b,c]) == [single_embed(a), single_embed(b), single_embed(c)]` bit-for-bit. This is the only effective verification of the right-pad offset math underlying the batched-inference optimization.
- **`POST /v1/embeddings` HTTP handler** integration test (web service fixture).
- **`EmbeddingsSession::Embed()` validation paths** (non-tensor item, count mismatch) — small unit tests using a mock session.
- **C++ `EmbeddingsSession` ctor task validation** — should reject non-embedding models.
- **Session factory branch test** — `Session::Create` returns `EmbeddingsSession` for `task == "text-embedding"`.
- **WebGPU EP bootstrapper** unit tests for platform CDN URL/binary table.
- **`FoundryLocalSetCatalogRegion` C ABI** round-trip test.

Items 1-2 can be done immediately with zero risk. Item 3 (embeddings) is the big-ticket
item — it's a new feature end-to-end requiring session, web endpoint, contract types, and
C ABI exposure. Item 5 (WebGPU bootstrapper) is mechanical — copy the CUDA bootstrapper
pattern.

---

## Architecture Notes for Embeddings

The embedding feature follows the existing **Item-in / Item-out / Session-hosted-model**
pattern used by `ChatSession` and `AudioSession`. Consistency across request types is
the priority — when predictive inference is added later (using ORT's stateless
`InferenceSession` directly), it will follow the same shape.

```
C ABI layer:
  - Session::Create(model) returns an EmbeddingsSession when model.task_type == embeddings
  - No new C ABI entry points needed — uses the existing FlInference_SessionProcess
    flow with TextItem inputs and TensorItem outputs

Internal:
  - EmbeddingsSession : Session
      - allow_concurrent_requests = true (each request is fully stateless)
      - Holds reference to the loaded GenAIModelInstance via ModelLoadManager
      - ProcessRequestImpl creates a fresh OgaGenerator per request (cheap;
        the underlying model + tokenizer are already loaded)
  - GenAIConfig.hidden_size for output dimension
  - ORT GenAI flow per input string:
      Tokenizer.Encode → append EOS → GeneratorParams (max_length = N+1)
      → Generator → GenerateNextToken → GetOutput("hidden_states")
      → check tensor element type → FP16→FP32 if needed
      → last-token pool → L2-normalize → TensorItem

Web service:
  - EmbeddingsHandler: POST /v1/embeddings
  - Deserialize → build Request with TextItem(s) → ProcessRequest
  - Read TensorItem outputs from Response → serialize to EmbeddingCreateResponse
  - Contract types: src/contracts/embeddings.h + JSON converters

Key decisions (resolved):
  1. Session type: EmbeddingsSession with allow_concurrent_requests = true.
     Session is the unifying abstraction — items in / items out / session
     hosts the model. Stateful is not a requirement; predictive inferencing
     will follow the same pattern using ORT's stateless InferenceSession.
     ModelLoadManager keeps the model loaded, so Session creation/destruction
     is cheap. We do not load/unload models per Session.
  2. Generator lifecycle: Created per-request inside ProcessRequestImpl. The
     model and tokenizer are owned by GenAIModelInstance and stay loaded.
  3. Input/output items:
     - Input: TextItem (single) or multiple TextItems (batch)
     - Output: one TensorItem (1-D float vector) per input
  4. FP16 handling: Check tensor element type via ORT GenAI's tensor API and
     convert FP16→FP32 for quantized models (matches the C# 352a76b0 fix).
  5. KV cache sizing: max_length = token_count + 1 to avoid oversized
     allocations on memory-constrained hardware (matches C# optimization).
```

---

## Diff from Previous Plan

Items carried forward from the previous plan's "Design Input" / "Deferred" sections:

| Previous Item | Status Now |
|---------------|-----------|
| EP detection design (#6) | **Done** — full implementation shipped |
| Download UX improvements (#7) | Partially done (retry/backoff done, concurrency config still missing) |
| Realtime audio streaming (#8) | **Done** — full implementation shipped |
| Download timeout config (#9) | Still TODO — low priority |
| Download concurrency config | Still TODO — captured in design input |

New items not in previous plan:
- **Embeddings** (new feature, didn't exist when previous plan was written)
- **Download region parameter** (new feature)
- **WebGPU EP bootstrapper** (was hinted at in EP detection design input, now concrete)
