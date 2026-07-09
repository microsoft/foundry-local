# EP Detection & Bootstrapping — C++ Implementation Plan

## Overview

Replace the stubbed `EpDetector` (CPU-only) with real hardware detection, EP
discovery, and on-demand EP download/registration. This enables the C++ SDK to
use GPU, NPU, and other accelerators via the WinML EP catalog and manual CUDA
bootstrapping.

**C# reference implementation:**
- `D:\src\github\neutron.main\src\Service\Contracts\IEpDetector.cs`
- `D:\src\github\neutron.main\src\Service\Providers\Detector\EpDetector.cs`
- `D:\src\github\neutron.main\src\Service\Providers\Detector\CudaEpBootstrapper.cs`
- `D:\src\github\neutron.main\src\Service\Providers\Detector.WinAppSDK\WinMLEpBootstrapper.cs`
- `D:\src\github\neutron.main\src\FoundryLocalCore\Core\FoundryLocalCore.cs`

---

## Architecture

```
C ABI (foundry_local_c.h)
  └── IEpDetector interface (expanded)
        ├── EpDetector (orchestrator — replaces stub)
        │     ├── WinMLEpBootstrapper (Windows 10 19H1+ reg-free, WinML 2.x package present)
        │     ├── CudaEpBootstrapper (Windows x64 + NVIDIA, Linux x64)
        │     └── [future bootstrappers as needed]
        └── StubEpDetector (fallback — CPU only, used in tests)
```

### Key Design Principles

1. **Two-step explicit flow.** `GetDiscoverableEps()` returns metadata, then
   `DownloadAndRegisterEps()` does the actual work. No auto-registration at startup.
2. **Catalog invalidation after EP registration.** After new EPs register, the model
   catalog clears its cache so the next query reflects new hardware capabilities.
3. **Graceful degradation.** If WinML isn't available (older OS, missing DLL), fall
   back to CPU-only. The SDK always works — EPs just add hardware acceleration.
4. **No blocking on EP registration in catalog init.** Catalog returns models for
   currently available EPs. Callers re-query after EP registration.

---

## Dependencies

### Microsoft.Windows.AI.MachineLearning NuGet (WinML 2.x)

**Package:** `Microsoft.Windows.AI.MachineLearning` 2.1.70 (or newer GA)

WinML 2.x is reg-free: the package ships a single self-contained native DLL
that loads directly on Windows 10 19H1 (build 18362) and later. There is no
Windows App SDK bootstrap step.

**What we need from the package:**

| Asset | Package Path | Purpose |
|-------|-------------|---------|
| `WinMLEpCatalog.h` | `include/` | C API for EP catalog — enumerate, query, download, register |
| `WinMLAsync.h` | `include/` | Async block + progress callback infrastructure |
| `Microsoft.Windows.AI.MachineLearning.lib` | `lib/native/x64/` | Import library (delay-loaded) |
| `Microsoft.Windows.AI.MachineLearning.dll` | `runtimes/win-x64/native/` | Runtime DLL (deploy alongside) |

**What we do NOT use from the package:**

| Asset | Why Not |
|-------|---------|
| `onnxruntime.dll` / `onnxruntime.lib` | We use our own ORT from vcpkg. ORT ABI is stable ≥1.23. |
| `onnxruntime_providers_shared.dll` | Same — our ORT build |
| `DirectML.dll` | Loaded by WinML EPs, not by us directly |
| `WindowsMLAutoInitializer.cpp` | We manage ORT initialization ourselves |
| `WinMLModelCatalog.h` | We have our own Azure model catalog |

**Confirmed:** The WinML EP catalog API is independent of the WinML-bundled ORT.
We load our own ORT eagerly in `Manager::Create()`. EP registration via
`RegisterExecutionProviderLibrary()` goes through our ORT instance. This is safe
because ORT's C API is ABI-stable.

### WinML EP Catalog C API Summary

The `Microsoft.Windows.AI.MachineLearning` 2.1.70 package provides a pure C API (no WinRT/C++/WinRT projection needed):

```c
// Catalog lifecycle
STDAPI WinMLEpCatalogCreate(WinMLEpCatalogHandle* catalog);
STDAPI_(void) WinMLEpCatalogRelease(WinMLEpCatalogHandle catalog);

// Enumerate all available EPs
STDAPI WinMLEpCatalogEnumProviders(WinMLEpCatalogHandle catalog,
                                    WinMLEpEnumCallback callback, void* context);

// Per-EP info
STDAPI WinMLEpGetName(WinMLEpHandle ep, ...);
STDAPI WinMLEpGetReadyState(WinMLEpHandle ep, WinMLEpReadyState* state);
STDAPI WinMLEpGetLibraryPath(WinMLEpHandle ep, ...);
STDAPI WinMLEpGetCertification(WinMLEpHandle ep, WinMLEpCertification* cert);

// Download + register (async with progress)
STDAPI WinMLEpEnsureReadyAsync(WinMLEpHandle ep, WinMLAsyncBlock* async);
```

**C# → C API mapping:**

| C# (WinRT projection) | C API (`WinMLEpCatalog.h`) |
|------------------------|---------------------------|
| `ExecutionProviderCatalog.GetDefault()` | `WinMLEpCatalogCreate()` |
| `catalog.FindAllProviders()` | `WinMLEpCatalogEnumProviders()` |
| `provider.EnsureReadyAsync()` | `WinMLEpEnsureReadyAsync()` |
| `provider.Name` | `WinMLEpGetName()` |
| `provider.ReadyState` | `WinMLEpGetReadyState()` |
| `provider.LibraryPath` | `WinMLEpGetLibraryPath()` |
| `provider.Certification` | `WinMLEpGetCertification()` |

---

## Implementation Phases

> **Status:** All 7 phases complete. 638 internal + 6 SDK integration tests passing. C# bindings wired.
>
> Deviations from original plan noted inline with **[Deviation]** markers.
>
> **Testing Trophy applied:** Redundant unit tests pruned after SDK integration tests were
> added. Internal EP unit tests now cover only download orchestration logic (mock-based),
> which can't be safely exercised via integration tests. Discovery and query paths are
> tested through the full stack in `test/sdk_api/ep_detection_test.cc`.

### Phase 1: NuGet Acquisition for Microsoft.Windows.AI.MachineLearning ✅

**Goal:** Make the WinML headers and import lib available to CMake.

**Approach:** FetchContent-based download of the WinML 2.x NuGet package, no vcpkg port needed.

**What we use from the package:**
- `WinMLEpCatalog.h`, `WinMLAsync.h` → include path
- `Microsoft.Windows.AI.MachineLearning.lib` → import library (x64, arm64)
- `Microsoft.Windows.AI.MachineLearning.dll` → copied to output dir for deployment

**What we exclude:**
- All ORT files (`onnxruntime.dll`, `onnxruntime.lib`, etc.) — Foundry-Local ships its own ORT
- `DirectML.dll` (loaded by EPs themselves)
- Auto-initializer `.cpp` files
- Model catalog headers (we have our own)

**CMake integration:**
- `find_package(WinMLEpCatalog)` — gated behind `if(WIN32)` (always enabled on Windows)
- Windows-only.
- Linked with `/DELAYLOAD:Microsoft.Windows.AI.MachineLearning.dll` — the DLL may
  not be present on older systems

**Validation:** Write a minimal test that calls `WinMLEpCatalogCreate()` +
`WinMLEpCatalogEnumProviders()` and prints discovered EPs. Run on Windows 10 19H1+.

---

### Phase 2: Expand Interfaces ✅

**[Deviation]:** `ProgressCallback` returns `bool` (true=continue, false=cancel) instead of `int`. `cancel_flag` removed from all interfaces — cancellation is signaled via callback return value, tracked by `EpDetector` orchestrator. `GetAvailableDevicesToEPs()` and `GetDiscoverableEps()` return `const&` to avoid per-call container construction.

#### IEpBootstrapper (internal)

```cpp
// src/ep_detection/ep_bootstrapper.h
class IEpBootstrapper {
 public:
  virtual ~IEpBootstrapper() = default;
  virtual const std::string& Name() const = 0;
  virtual bool IsRegistered() const = 0;

  /// Downloads and registers the EP. Returns true on success.
  /// progress_cb receives (ep_name, percent 0-100). Return non-zero to cancel.
  virtual bool DownloadAndRegister(
      bool force,
      std::atomic<bool>* cancel_flag,
      std::function<int(const char* ep_name, float percent)> progress_cb) = 0;
};
```

#### Expanded IEpDetector

```cpp
// src/ep_detection/ep_detector.h

struct EpInfo {
  std::string name;
  bool is_registered;
};

struct EpDownloadResult {
  bool success;
  std::string status;
  std::vector<std::string> registered_eps;
  std::vector<std::string> failed_eps;
};

class IEpDetector {
 public:
  virtual ~IEpDetector() = default;

  // Existing — device-to-EP mapping for catalog filtering
  virtual std::map<std::string, std::vector<std::string>>
      GetAvailableDevicesToEPs() const = 0;

  // New — discovery
  virtual std::vector<EpInfo> GetDiscoverableEps() const = 0;

  // New — download and register (blocking). names == nullptr means all.
  virtual EpDownloadResult DownloadAndRegisterEps(
      const std::vector<std::string>* names,
      std::atomic<bool>* cancel_flag,
      std::function<int(const char* ep_name, float percent)> progress_cb) = 0;

  // New — query download state
  virtual bool IsDownloadInProgress() const = 0;
};
```

**StubEpDetector** stays as the default for tests and platforms without EP support.
New methods return empty/no-op.

---

### Phase 3: WinMLEpBootstrapper ✅

**File:** `src/ep_detection/winml_ep_bootstrapper.h/.cc` (Windows-only)

**Design:**
- Each instance wraps a single `WinMLEpHandle` obtained from catalog enumeration.
- `Name()` returns the EP name captured during enumeration.
- `IsRegistered()` returns whether the EP has been successfully registered with ORT
  (tracked via a `registered_` flag flipped on by `DownloadAndRegister`). Note this
  is ORT-registration state, not WinML readyState — an EP can be `Ready` in the
  WinML catalog but not yet registered with ORT.
- `DownloadAndRegister()` → `WinMLEpEnsureReadyAsync()` with a `WinMLAsyncBlock`
  whose progress callback forwards WinML's progress value (a percentage in
  0–100, matching the WinRT `IAsyncOperationWithProgress<_, double>` projection)
  straight through to the caller's percent callback after clamping, then
  `WinMLAsyncGetStatus(async, TRUE)` for a synchronous wait. Caller-requested
  cancellation goes through `WinMLAsyncCancel`.

**Discovery (static factory):**
```cpp
// Returns one bootstrapper per discovered WinML EP. Empty if unavailable.
// register_ep is plumbed through construction time so each bootstrapper can
// register its library with ORT inside DownloadAndRegister().
static std::vector<std::unique_ptr<WinMLEpBootstrapper>>
    DiscoverProviders(EpRegistrationCallback register_ep, ILogger& logger);
```

**Implementation:**
1. Query Windows build number via `RtlGetVersion` for diagnostics only — never gates
   behavior. Gating happens at `WinMLEpCatalogCreate()` (DLL load failure → empty).
2. `WinMLEpCatalogCreate()` — if this fails (DLL not found / delay-load failure),
   log info, return empty. Not an error.
3. `WinMLEpCatalogEnumProviders()` — callback collects `WinMLEpHandle` + `WinMLEpInfo`.
4. Create one `WinMLEpBootstrapper` per EP.
5. Catalog handle released when bootstrappers are done (shared via `shared_ptr`).

**MIGraphX special case:** Same as C# — treat failure as non-fatal. MIGraphX isn't
available in retail builds yet.

---

### Phase 4: CudaEpBootstrapper ✅

**File:** `src/ep_detection/cuda_ep_bootstrapper.h/.cc`

**Port of:** `CudaEpBootstrapper.cs`

**Design:**
- Downloads CUDA EP provider DLLs from Azure CDN.
- Cross-process file-based lock (`cuda-ep.lock`) to prevent concurrent installs.
- SHA256 verification of downloaded binaries.
- `SetDllDirectory()` + ORT `RegisterExecutionProviderLibrary()` for registration.

**Platform behavior:**
- **Windows x64 + NVIDIA GPU:** Download from CDN, verify, register.
- **Linux x64:** Register from co-located `.so` in app package (no download).
- **Other platforms:** Not applicable — bootstrapper not created.

**Download flow (from C#):**
1. Check if already registered → return true.
2. Acquire file lock (`cuda-ep.lock`, 10-minute timeout).
3. Check if package exists and SHA256 matches → skip download.
4. Download ZIP from Azure CDN URL (with progress callback).
5. Extract to cache directory.
6. Verify extracted binaries match expected hashes.
7. `SetDllDirectory(extracted_path)` + `RegisterExecutionProviderLibrary()`.
8. Release lock.

**Dependencies:**
- Uses our existing `DownloadManager` for HTTP downloads (or direct HTTP if simpler).
- SHA256 via platform crypto (Windows `BCryptHashData`, Linux `<openssl/sha.h>`,
  or a lightweight embedded impl).
- File locking via `LockFileEx` (Windows) / `flock` (Linux).

**Download URLs and hashes:** Hardcoded per release, matching the C# source.
These change when the CUDA EP package is updated.

---

### Phase 5: EpDetector Orchestrator ✅

**[Deviation]:** OrtEnv acquired as singleton via `CreateEnv` (returns existing instance). `EpRegistrationCallback` created in `Manager::Create()` and passed through to bootstrappers. No catalog invalidation callback yet — deferred until catalog caching is implemented.

**File:** `src/ep_detection/ep_detector.h/.cc` (replace existing stub)

**Port of:** `EpDetector.cs`

**Design:**
- Owns a vector of `unique_ptr<IEpBootstrapper>`.
- Constructor discovers bootstrappers:
  1. `WinMLEpBootstrapper::DiscoverProviders()` → one per WinML EP (Windows only)
  2. `CudaEpBootstrapper` → if NVIDIA GPU detected (Windows x64) or Linux x64
- `GetAvailableDevicesToEPs()` → queries ORT `GetEpDevices()` (replaces hardcoded
  CPU-only map). Falls back to `{"CPU": ["CPUExecutionProvider"]}` if ORT env not
  available.
- `GetDiscoverableEps()` → maps bootstrappers to `EpInfo` structs.
- `DownloadAndRegisterEps()` → orchestrates bootstrapper sequence:
  - Filter by requested names (or all).
  - Process sequentially with 30-minute timeout.
  - Track succeeded/failed.
  - On any success → **invalidate catalog cache**.
  - Thread-safe via mutex.

**Catalog invalidation:**
- `EpDetector` receives a `std::function<void()> invalidate_catalog_cb` at
  construction (or a reference to `IModelCatalog`).
- Called after any EP successfully registers, so the next `GetAvailableModels()`
  re-queries with new EP awareness.

**NvTensorRTRTX → CUDA dependency:** Same as C# — if NvTensorRTRTXExecutionProvider
is requested, auto-add CUDAExecutionProvider to the download set (unless already
registered).

---

### Phase 6: C ABI Surface ✅

**[Deviation]:** Functions added to the root `flApi` vtable (not a sub-API). Uses parallel arrays (names + is_registered + count) for `GetDiscoverableEps` instead of an opaque `flEpInfoArray`. `DownloadAndRegisterEps` uses `flEpProgressCallback` (returns 0=continue, non-zero=cancel). C# bindings complete in `NativeMethods.cs`, `FoundryLocalApi.cs`, `FoundryLocalManager.cs`.

Add to the `flManagerApi` vtable in `foundry_local_c.h`:

```c
/// EP discovery — returns array of EpInfo structs.
flStatusPtr FL_API_T(GetDiscoverableEps,
    _In_ const flManager* manager,
    _Outptr_ flEpInfoArray** out);

/// EP download and registration. Blocking. names=NULL for all.
/// Progress callback receives ep_name + percent. Return non-zero to cancel.
flStatusPtr FL_API_T(DownloadAndRegisterEps,
    _In_ flManager* manager,
    _In_opt_ const char* const* names,
    _In_ uint32_t name_count,
    _In_opt_ flEpProgressCallback progress_cb,
    _In_opt_ void* progress_context);

/// Query if EP download is in progress.
void FL_API_T(IsEpDownloadInProgress,
    _In_ const flManager* manager,
    _Out_ bool* in_progress);
```

**C++ wrapper:**
```cpp
std::vector<EpInfo> Manager::GetDiscoverableEps() const;
EpDownloadResult Manager::DownloadAndRegisterEps(
    const std::vector<std::string>* names,
    std::function<int(const char*, float)> progress_cb);
bool Manager::IsEpDownloadInProgress() const;
```

**C# bindings:** Add to `NativeMethods.cs`, `FoundryLocalApi.cs`,
`FoundryLocalManager.cs` — mirrors existing pattern for other Manager methods.

---

### Phase 7: ModelLoadManager EP Guard ✅

**[Deviation]:** `HasEP()` queries `IEpDetector` live instead of using a cached list, so newly-registered EPs are immediately visible. Added `EPUtils::EPtoRegistrationName()` for EP enum → registration string mapping. Two-path guard: (1) explicit EP resolved → check via registration name, (2) model_id substring table for device hints when EP is `kDefault`.

Port the EP availability check from C# `ModelManager`:

```cpp
// In ModelLoadManager::ResolveExecutionProvider()
// Before loading, verify the model's required EP is available.
static const std::map<std::string, std::string> kModelIdToRequiredEP = {
    {"cuda-gpu", "CUDAExecutionProvider"},
    {"openvino-npu", "OpenVINOExecutionProvider"},
    {"openvino-gpu", "OpenVINOExecutionProvider"},
    {"qnn-npu", "QNNExecutionProvider"},
    {"trtrtx-gpu", "NvTensorRTRTXExecutionProvider"},
    {"vitis-npu", "VitisAIExecutionProvider"},
};

// Check model_id against table. If match found, verify HasEP(required_ep).
// If not available, throw with actionable message:
//   "Model 'xyz-cuda-gpu:4' requires CUDAExecutionProvider which is not
//    registered. Call DownloadAndRegisterEps() first."
```

---

## Risks & Open Questions

| Risk | Mitigation |
|------|------------|
| `Microsoft.Windows.AI.MachineLearning.dll` not present on every Windows install | `WinMLEpBootstrapper::DiscoverProviders` probes via `LoadLibraryW` and returns empty when the DLL is missing — no WinML bootstrappers join the discovery set |
| CUDA download URLs / hashes change per release | Hardcode per release, mirroring C# pattern |
| ORT `GetEpDevices()` not available in our ORT build | Check ORT version/API availability. Fallback to CPU-only device map |
| Cross-platform CUDA EP registration | Windows: download from CDN. Linux: register from co-located `.so` |

---

## File Layout

```
src/ep_detection/
  ep_detector.h          — IEpDetector (expanded), EpInfo, EpDownloadResult, EpDetector
  ep_detector.cc         — EpDetector orchestrator (replaces current stub)
  ep_bootstrapper.h      — IEpBootstrapper interface
  winml_ep_bootstrapper.h/.cc  — WinML EP catalog wrapper (Windows-only)
  cuda_ep_bootstrapper.h/.cc   — CUDA EP download + register
  webgpu_ep_bootstrapper.h/.cc — WebGPU EP download + register
  runtime_version_info.h/.cc   — ORT runtime version + per-EP version (logged at INFO)
```

---

## Dependencies on Other Work

- **Catalog invalidation** — new method on `BaseModelCatalog` / `AzureModelCatalog`.
  Not yet implemented but straightforward.

---

## Testing Strategy

> **Testing Trophy:** Integration tests are the primary coverage layer. Unit tests
> are reserved for hard-to-reach orchestration logic. See the rationale in the
> file header of `test/internal_api/ep_detector_test.cc`.

### SDK Integration Tests (`test/sdk_api/ep_detection_test.cc`) — 6 tests

Exercise the **full stack** through the public C++ API: C++ wrapper → C ABI → Manager → EpDetector → real WinML/CUDA bootstrappers.

| Test | What it validates |
|------|-------------------|
| `GetDiscoverableEps_ReturnsNonEmpty` | Real EP discovery works end-to-end |
| `GetDiscoverableEps_EpInfoHasNonEmptyNames` | EP metadata is populated |
| `GetDiscoverableEps_EpsNotRegisteredBeforeDownload` | Correct initial state |
| `GetAvailableDevicesToEPs_IncludesCpu` | CPU always present in device map |
| `GetAvailableDevicesToEPs_CpuEPIsCorrect` | CPU EP has canonical name |
| `IsEpDownloadInProgress_InitiallyFalse` | Correct initial download state |

### Internal Unit Tests (`test/internal_api/ep_detector_test.cc`) — 8 tests

Mock-based tests for **download orchestration logic** that can't be exercised via integration tests (would trigger real EP downloads).

| Test | What it validates |
|------|-------------------|
| `AfterRegister_GpuDevicesAppear` | Device map updates after registration |
| `DownloadAll_CallsAllBootstrappers` | All bootstrappers invoked |
| `DownloadFiltered_CallsOnlyMatchingBootstrapper` | Name filtering works |
| `DownloadAll_WhenOneFailsResultHasFailedEps` | Partial failure reporting |
| `DownloadAll_ProgressCallbackForwarded` | Callback wiring |
| `DownloadAll_DiscoverableEpsReflectRegistrationState` | State mutation after registration |
| `DownloadFiltered_UnknownNamesSkipped` | Unknown names silently ignored |
| `DownloadFiltered_AllNamesUnknown_SucceedsWithNothing` | All-unknown edge case |

### ModelLoadManager EP Guard (`test/internal_api/model_load_manager_test.cc`) — 7 tests

| Test | What it validates |
|------|-------------------|
| EP guard blocks load for missing EP | Actionable error message |
| EP guard allows load when EP is available | Model loads successfully |
| `kModelIdEpRequirements` substring matching | Device hints in model_id |
