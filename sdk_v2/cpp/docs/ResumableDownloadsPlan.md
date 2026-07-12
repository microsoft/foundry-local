# Resumable Downloads — C++ Port Plan

> Plan for porting the cross-process safe, resumable model download functionality
> from C# (`dev/FoundryLocalCore/main`) to the C++ SDK. Derived from analysis of
> PR 14807315 ("Merge resumable download changes") and the follow-up PRs that
> shaped the final design.

## Source of Truth

The plan aligns to the **latest** state of `dev/FoundryLocalCore/main`
(tip `7cd71f8d`, AzureExtensions last touched by `7310bdbf` on 2026-05-13), **not**
the original PR 14807315 (`ea37e85a`). Several aspects of the PR were reverted
or simplified by subsequent commits — those are noted below so we don't port
removed features.

| C# Commit | Effect on this plan |
|-----------|---------------------|
| `ea37e85a` PR 14807315 | Introduced `CrossProcessFileLock`, `BlobDownloadState`, `.download.progress`, multi-file parallelism, 4 MB chunks |
| `ca16c0ea` PR 15200460 | **Removed** `.download.progress` + "report progress from waiting process" |
| `67b7d4e8` PR 15171866 | Reverted multi-file `Parallel.ForEachAsync` → sequential `foreach`; reverted 4 MB → 2 MB chunks; consolidated `MaxConcurrency` static |
| `f02d5151` / `7310bdbf` | Added region-based download (orthogonal — tracked separately) |
| `a4c019db` | Added `OnDownloadComplete` telemetry record (out of scope — C++ telemetry is stubbed) |

## Current C++ State

`sdk_v2/cpp/src/download/blob_downloader.{h,cc}` implements `AzureBlobDownloader`
and the top-level `DownloadBlobsToDirectory`. It already has:

- Sort smallest-first
- 0% progress emit at start
- Configurable per-blob concurrency (Android 8, default 64) via
  `DownloadManager` constructor (wired from `Configuration::NumModelDownloadThreads`)
- Retry with exponential backoff (via Azure SDK `BlobClientOptions::Retry`)
- 2 MB minimum chunk size
- Per-blob progress callback bridged to a `std::atomic<bool>` cancellation flag

It does **not** have:

- Cross-process file lock for the model directory
- Per-chunk resumable download state (bitmap of completed chunks persisted to disk)
- "Already downloaded" short-circuit (no `IsDownloadNeeded_File` equivalent)
- True linked cancellation across the chunk batch (current batch waits for in-flight
  futures before cancelling the next batch)

## Goals

1. Multiple processes on the same machine cannot corrupt a shared model cache by
   concurrently writing to the same directory.
2. Interrupted downloads (crash, kill, network outage) resume from where they
   stopped rather than restarting from byte zero.
3. Re-running a download of a model that is already fully cached is effectively
   free — no re-enumeration cost on the per-blob path beyond a `stat`.
4. A non-retryable failure on one chunk cancels the rest of the blob's chunks
   immediately, instead of waiting for the current batch to drain.

## Non-Goals

- `.download.progress` file / progress-from-waiting-process. The latest C# removed
  this. Waiting processes simply get a single log line and no progress callbacks
  until they acquire the lock and start downloading themselves.
- Multi-file parallel download. The latest C# is sequential per-blob.
- Region-based download (`ModelRegistryRegion`). Tracked as a separate item.
- `OnDownloadComplete` telemetry. Defer until C++ telemetry is implemented.

---

## Increments

The work splits into three independent increments. Each leaves the SDK in a
shippable state. **No public C ABI changes** in any increment.

### Increment 1 — Cross-process lock + "already downloaded" skip

**Files added**

- `sdk_v2/cpp/src/platform/cross_process_file_lock.h`
- `sdk_v2/cpp/src/platform/cross_process_file_lock.cc`

**Files modified**

- `sdk_v2/cpp/src/download/blob_downloader.{h,cc}` — skip-existing check inside
  `DownloadBlobsToDirectory`.
- `sdk_v2/cpp/src/download/download_manager.cc` — acquire/wait for the model
  lock around the existing download call.

**`CrossProcessFileLock` API**

```cpp
namespace fl {

/// RAII exclusive lock backed by an OS-level file lock on `<dir>/.download.lock`.
/// Use `TryAcquireForDirectory` to attempt non-blocking acquisition; the returned
/// pointer is null if another process holds the lock. The lock is released and
/// the file is removed on destruction.
class CrossProcessFileLock {
 public:
  /// Returns nullptr if the lock is currently held by another process.
  /// Throws fl::Exception on unexpected errors (permission denied, etc.).
  static std::unique_ptr<CrossProcessFileLock> TryAcquireForDirectory(
      const std::filesystem::path& dir);

  ~CrossProcessFileLock();

  CrossProcessFileLock(const CrossProcessFileLock&) = delete;
  CrossProcessFileLock& operator=(const CrossProcessFileLock&) = delete;

 private:
  // platform-specific state (HANDLE on Windows, int fd on POSIX)
};

}  // namespace fl
```

**Platform implementations**

- **Windows:** `CreateFileW` with `dwShareMode = 0`,
  `dwCreationDisposition = OPEN_ALWAYS`, `dwFlagsAndAttributes = FILE_FLAG_DELETE_ON_CLOSE`.
  Sharing-violation HRESULTs (`ERROR_SHARING_VIOLATION` = 0x20,
  `ERROR_LOCK_VIOLATION` = 0x21) → return null. Anything else → throw.
- **POSIX (Linux, macOS, Android):** `open(O_CREAT | O_RDWR, 0644)` then
  `flock(fd, LOCK_EX | LOCK_NB)`. `EWOULDBLOCK` / `EAGAIN` → return null. On
  destruction, `unlink` the file before `close` so a crash leaves a stale (but
  harmless) zero-byte file that the next process will simply re-acquire.
- Write `PID:<pid>,Time:<iso8601>\n` to the file for post-mortem debugging.

**Wait-for-lock helper** (in `download_manager.cc` or a small helper in the
lock TU):

```cpp
/// Polls TryAcquireForDirectory every 1.25 s. Throws fl::Exception with
/// FOUNDRY_LOCAL_ERROR_TIMEOUT after 3 hours.
std::unique_ptr<CrossProcessFileLock> WaitForLock(
    const std::filesystem::path& dir,
    const std::atomic<bool>& cancelled);
```

`DownloadManager::DownloadModel` flow becomes:

```
ensure output dir exists
lock = TryAcquireForDirectory(output_dir)
if (!lock) {
  log "Model download is being performed by another process. Waiting..."
  lock = WaitForLock(output_dir, cancelled)
}
// existing enumerate + download path
```

**"Already downloaded" skip** in `DownloadBlobsToDirectory`:

Currently the filter loop only checks the path prefix and `inference_model.json`
filter. Add:

```cpp
bool IsDownloadNeeded(const BlobItemInfo& blob, const std::filesystem::path& local) {
  // (Increment 2 will also check for a .dlstate sidecar here.)
  std::error_code ec;
  if (std::filesystem::exists(local, ec) &&
      std::filesystem::file_size(local, ec) == blob.content_length) {
    return false;
  }
  return true;
}
```

Files that are skipped contribute to `total_size` (so progress denominator is
correct) and their bytes are credited as an immediate `progress(skipped/total*100)`
call before the download loop starts — matching the existing C# `TotalProgressAdapter`
behavior.

**Tests** (`sdk_v2/cpp/test/internal_api/cross_process_file_lock_test.cc`)

Mirror `CrossProcessFileLockTests.cs`:

1. `TryAcquire_ReturnsLock_WhenAvailable`
2. `TryAcquire_ReturnsNull_WhenHeldByAnotherProcessHandle` (Windows: open second
   handle in same process; POSIX: `flock` is per-fd, so use `fork()` — gate this
   test on `__linux__ || __APPLE__`)
3. `Lock_IsReleasedOnDestruction`
4. `Lock_FileIsRemovedAfterRelease`
5. `Lock_HandlesPreexistingStaleFile` (touch the file first, then acquire)
6. `WaitForLock_AcquiresAfterReleaserDestructs` (release on a background thread
   after ~500 ms; verify wait returns)
7. `WaitForLock_RespectsCancellation` (set the cancelled flag mid-wait)

Plus `blob_downloader_test.cc` additions:

- `DownloadBlobsToDirectory_SkipsExistingFilesWithCorrectSize`
- `DownloadBlobsToDirectory_RedownloadsFilesWithWrongSize`
- `DownloadBlobsToDirectory_ReportsSkippedBytesInInitialProgress`

---

### Increment 2 — Per-chunk resume (`BlobDownloadState`) + linked cancellation

**Files added**

- `sdk_v2/cpp/src/download/blob_download_state.h`
- `sdk_v2/cpp/src/download/blob_download_state.cc`

**Files modified**

- `sdk_v2/cpp/src/download/blob_downloader.cc` — replace the current chunk loop
  in `AzureBlobDownloader::DownloadBlob` with the resume-aware variant; tighten
  cross-batch cancellation.
- `sdk_v2/cpp/src/download/blob_downloader.{h,cc}` — extend `IsDownloadNeeded`
  to return `true` (resume) when a `.dlstate` sidecar exists.

**`BlobDownloadState`** — direct port of `BlobDownloadState.cs`:

```cpp
namespace fl {

class BlobDownloadState {
 public:
  // Persisted DTO fields
  std::string blob_name;
  std::string local_file_path;
  int64_t blob_size = 0;
  int chunk_size = 0;
  int total_chunks = 0;
  std::chrono::system_clock::time_point last_modified;
  std::vector<uint64_t> completion_bitmap;  // packed bits, 64 chunks per word
  int completed_count = 0;

  // Path of the sidecar JSON file for `local_file_path`.
  static std::filesystem::path StateFilePath(const std::filesystem::path& local);

  // Load existing state. Returns nullopt if missing or unparseable.
  static std::optional<BlobDownloadState> Load(const std::filesystem::path& local);

  // True iff (blob_name, blob_size, chunk_size, total_chunks) match `expected`.
  bool MatchesParameters(const BlobDownloadState& expected) const;

  bool IsChunkComplete(int chunk_index) const;
  void MarkChunkComplete(int chunk_index);        // updates completed_count

  bool IsComplete() const { return completed_count == total_chunks; }
  int64_t CalculateDownloadedSize(int64_t blob_size) const;  // ported fixed version

  std::vector<int> GetPendingChunks() const;

  void Save() const;   // atomic write via temp + rename
  static void Delete(const std::filesystem::path& local);
};

}  // namespace fl
```

Serialization uses `nlohmann::json` (already a dependency). Sidecar path is
`<local_file_path>.dlstate` (or whatever name C# uses — verify
`BlobDownloadState.GetStateFilePath`; the C++ name should match so a hand-off
between languages works if that ever becomes a use case).

**Important:** port the corrected `CalculateDownloadedSize` (post-PR
14807315 — uses `% ULONG_BITS == 0`, not `!= 0`). Don't recreate the original
C# bug.

**Reworked `AzureBlobDownloader::DownloadBlob`** flow:

```
compute chunk_size (2 MB min) and num_chunks
state = BlobDownloadState::Load(local).value_or(fresh)
if state matches and state.IsComplete():
  log + return
if fresh:
  pre-allocate file (existing seek-and-put-zero is fine)
  state = fresh

report bytes_completed = state.CalculateDownloadedSize(blob_size)

pending = state.GetPendingChunks()
save_interval = max(10, num_chunks / 50)        // every ~2%
chunks_since_save = 0
failed = atomic<bool>{false}
ctx = Azure::Core::Context{}

// linked cancellation: a single ctx that is Cancel()'d on any chunk failure
parallel loop over `pending` with bounded concurrency (existing max_concurrency):
  if failed.load() || cancelled flag set: return
  try download range:
    write at offset (existing mutex-guarded write)
    {
      lock(state_mutex);
      state.MarkChunkComplete(idx)
      chunks_since_save++
      if chunks_since_save >= save_interval && !state.IsComplete():
        state.Save()
        chunks_since_save = 0
    }
  catch (cancelled): rethrow
  catch (other): failed = true; ctx.Cancel(); rethrow

if state.IsComplete():
  BlobDownloadState::Delete(local)
  return
if cancelled or failed:
  state.Save()
  throw
```

The existing C++ implementation already uses a futures+batch pattern. Convert
to a producer/consumer or a single shared atomic-counter pattern so the failure
of any chunk immediately stops everyone — no more "drain the current batch,
then notice we cancelled".

**Tests**

`blob_download_state_test.cc`:

1. `SaveLoad_RoundTrip`
2. `Load_ReturnsNullopt_OnMissingFile`
3. `Load_ReturnsNullopt_OnCorruptJson`
4. `MarkChunkComplete_UpdatesBitmapAndCount`
5. `CalculateDownloadedSize_HandlesPartialFinalWord` (covers the `% ULONG_BITS == 0` edge)
6. `GetPendingChunks_ReturnsOnlyUnsetBits`
7. `Delete_RemovesSidecar`

`blob_downloader_test.cc` additions:

1. `DownloadBlob_ResumesFromExistingState` — use a mock `IBlobDownloader` (or
   inject a chunk-failure hook) to: download N chunks, kill mid-flight, restart,
   verify only the missing chunks are re-requested.
2. `DownloadBlob_DeletesStateOnCompletion`
3. `DownloadBlob_PersistsStateOnFailure`
4. `DownloadBlob_FirstFailureCancelsRemainingInFlight` — verify with a mock
   that after the first chunk throws, no further chunk requests are issued.
5. `IsDownloadNeeded_TrueWhenStateSidecarPresent` — even if file size happens to match.

---

### Increment 3 — (Tracked separately) Region-based download

Not part of resumable-downloads scope but identified during analysis. Suggest
its own short plan covering:

- `Configuration::ModelRegistryRegion` ("auto" default)
- `ModelInfo::DetectedRegion` populated from the catalog response header
- `ModelRegistryClient` / `BlobDownloader` URL templated with the resolved region
- Container resolution started before/in parallel with lock acquisition

This is a non-trivial cross-cutting change and should be its own document.

---

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| `flock` semantics on POSIX are per-fd, not per-process; two `flock` calls in the same process from different fds both succeed | Test cross-process behavior with `fork()`; document that the lock is process-scoped (matches C# `FileShare.None` behavior). |
| `FILE_FLAG_DELETE_ON_CLOSE` on Windows can race with another process's `CreateFileW` (file deleted between open and lock) | Acceptable — losing the race just means returning null and the caller retries. C# has the same behavior. |
| Sidecar `.dlstate` file is inconsistent with the data file (e.g., crash between data write and state save) | The bitmap is the source of truth; a chunk marked incomplete is simply re-downloaded. A chunk marked complete but with stale data on disk would be a real corruption — but `Save` happens only **after** the write+flush of the chunk, so this window is small. We accept the same risk as C#. |
| JSON parse cost of `.dlstate` on large models (10 k+ chunks → ~80 KB bitmap as base64 or hex) | Bitmap stored as base64 of raw bytes, single `nlohmann::json` parse — measured in low ms. Match C#'s `JsonSourceGenerationOptions`. |
| Linked cancellation rework risks deadlock (waiting for a future that will never complete) | Use `std::async`'s future-wait inside a polling loop, or switch to a true bounded thread pool. Tests must include the failure path. |
| Sidecar files leak when a user deletes the model dir mid-download | Acceptable — the next download recreates state from scratch since the data file is also gone. |

---

## Out of Scope (Explicitly)

- **`.download.progress` file** — removed in C# PR 15200460. Don't port.
- **Multi-file `Parallel.ForEachAsync`** — reverted in C# PR 15171866. Don't port.
- **4 MB minimum chunk** — reverted in C# PR 15171866. Stay at 2 MB.
- **`OnDownloadComplete` telemetry** — C++ telemetry is stubbed; defer.
- **`ParserUtils.cs`** — RPC-only, no C++ counterpart.
- **Region-based download** — tracked as its own plan.

---

## Acceptance

- All new unit tests pass on Windows, Linux, and Android.
- Existing `blob_downloader_test.cc` tests still pass.
- Manual scenario test: start a download of a multi-GB model, kill the process
  mid-flight, restart — observe download resumes from previously completed chunks
  (log line `"Resuming download for {BlobName}: {Completed}/{Total} chunks already completed"`).
- Manual scenario test: two `sdk_integration_tests` processes concurrently
  request the same model — second process logs `"being performed by another
  process. Waiting..."` and completes successfully without re-downloading.
