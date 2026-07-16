# Handoff — Android TLS / SSL_CERT_FILE fix (Foundry-Local C++ Core)

Last updated: 2026-07-16. Written before a PC restart to preserve full context.

## TL;DR — status: FIX COMPLETE, VERIFIED ON-DEVICE (arm64), PUSHED

The Android app's C++ Core could not verify Azure TLS certs. Root cause: the
Core's bundled **libcurl ignores `SSL_CERT_FILE`**; every libcurl transport must
pass the CA bundle explicitly via `CURLOPT_CAINFO`. There were **four** such
transports. All four are now fixed and routed through one shared helper. Catalog
fetch AND model blob download both work on-device (arm64).

## Git state (everything is pushed — safe across restart)

Repo: `microsoft/Foundry-Local`. Main clone: `C:\projects\github\Foundry-Local`.

| Branch | Commit | Pushed | Contents |
|---|---|---|---|
| `android-ssl-cainfo-fix` | `3ba8e529` | ✅ origin | **THE FIX** (single, clean, single-concern commit) |
| `android-ssl-diagnostics` | `bd6b5bd6` | ✅ origin | Diagnostic-only: RegionFallback surfaces the real curl/TLS error (separate, optional PR) |
| `main` | `d7ba1574` | ✅ | clean |

Worktree for the fix: `C:\projects\github\Foundry-Local-worktrees\android-ssl-cainfo`
(branch `android-ssl-cainfo-fix`).

### Uncommitted (intentional, do NOT commit/merge)
- In the fix worktree: `sdk_v2/deps_versions.json` is downgraded
  `onnxruntime-genai` 0.14.1 → **0.14.0**. This is a *build-only, worktree-local*
  change so the Android build can run. **It must never ride along with the SSL
  fix commit.** Reason: v0.14.1 has **no Android AAR** published on GitHub
  Releases (404); v0.14.0 does (200). This is a shared cross-platform version
  file, so the downgrade is not a real fix — just a local workaround.

## The fix (commit 3ba8e529)

Root cause: the vcpkg libcurl 8.19.0 in the Core was built with a compiled-in
default CA path (`C:/projects/vcpkg/.../msys2/.../etc/ssl/cert.pem`, nonexistent
on Android) and without the fallback that would otherwise read `SSL_CERT_FILE`.
So `SSL_CERT_FILE` is silently ignored. `CURLOPT_CAINFO` is always honored.

Symptoms (two different errors, two different code paths):
- Catalog fetch: `self-signed certificate in certificate chain`
- Blob download: `unable to get local issuer certificate`

Fix — added `http::CaBundleFile()` (reads `SSL_CERT_FILE`) and routed all four
libcurl transports through it:
1. `sdk_v2/cpp/src/http/http_client.h` — new `CaBundleFile()` declaration.
2. `sdk_v2/cpp/src/http/http_client.cc` — `HttpRequestRaw` → `CurlTransportOptions.CAInfo`.
3. `sdk_v2/cpp/src/http/http_download.cc` — `HttpDownloadFile` → `CurlTransportOptions.CAInfo`.
4. `sdk_v2/cpp/src/download/blob_downloader.cc` — the Azure Storage SDK builds
   its OWN internal transport; added `MakeBlobClientOptions()` which installs a
   `CurlTransport(CAInfo)` on `BlobClientOptions.Transport.Transport`, applied at
   BOTH sites: `ListBlobs` and `DownloadBlob`.
- `sdk_v2/cpp/docs/AndroidBuildPlan.md` — SSL section corrected (previously
  wrongly claimed `SSL_CERT_FILE` is honored on its own).

Desktop Windows path (WinHTTP) is untouched — it uses the OS trust store.
Guard: `#if !defined(FOUNDRY_LOCAL_USE_WINHTTP_TRANSPORT)` (target-wide define,
only set under WIN32; Android gets the curl branch).

## Decisive proof gathered (why we trust the fix)
- On-device: 144-cert bundle + `SSL_CERT_FILE` set → still failed (adding roots
  changed nothing → file not read).
- On Windows: the 143-cert device bundle passed as `openssl s_client -CAfile`
  (== `CURLOPT_CAINFO`) verified `ai.azure.com` → `Verify return code: 0 (ok)`.
- Conclusion: bundle is fine; only the delivery mechanism matters. `CAInfo`
  works, `SSL_CERT_FILE` doesn't. (Also: `Microsoft TLS RSA Root G2` was a red
  herring — DigiCert Global Root G2, already in the store, anchors the chain.)

## Build (how to reproduce the .so)

Env:
- `VCPKG_ROOT=C:\projects\vcpkg`
- `ANDROID_NDK_HOME=C:\Users\shekadam\AppData\Local\Android\Sdk\ndk\29.0.14206865`
- `ninja` on PATH (`C:\ninja-win\ninja.exe`)

Command (ALWAYS via build.py, never `cmake --build` / never `--build_dir`):
```
cd C:\projects\github\Foundry-Local-worktrees\android-ssl-cainfo
python sdk_v2/cpp/build.py --android --android_abi arm64-v8a --config Release
python sdk_v2/cpp/build.py --android --android_abi x86_64 --config Release
```

### GenAI AAR gotcha
Fresh worktrees have no cached AAR and the 0.14.1 download 404s. Two options:
(a) keep the deps_versions.json 0.14.0 downgrade (done), and/or (b) seed the
cache by copying the ABI-agnostic AAR from the main tree:
`...\build\Android-<abi>\Release\_deps\genai-android-aar\` (copy the *contents*,
don't nest a folder). The AAR contains `jni/arm64-v8a` AND `jni/x86_64`.

## Output artifacts (built with the full fix, on disk — survive restart)
- arm64-v8a: `C:\projects\github\Foundry-Local-worktrees\android-ssl-cainfo\sdk_v2\cpp\build\Android-arm64-v8a\Release\bin\libfoundry_local.so`
- x86_64:    `C:\projects\github\Foundry-Local-worktrees\android-ssl-cainfo\sdk_v2\cpp\build\Android-x86_64\Release\bin\libfoundry_local.so`

Deploy: drop into the app's `jniLibs/<abi>/`.

## Open / next steps
- [ ] Open PR for `android-ssl-cainfo-fix`:
      https://github.com/microsoft/Foundry-Local/pull/new/android-ssl-cainfo-fix
- [ ] Decide whether to also PR `android-ssl-diagnostics` (independently useful).
- [ ] x86_64 `.so` built but NOT yet on-device/emulator tested (arm64 verified).
- [ ] Ensure the deps_versions.json downgrade is NOT included in the PR.
- [ ] Possibly file/track that onnxruntime-genai needs an Android AAR for 0.14.1.

## Side question answered this session — "AzureCatalogFilter" at config init
No type named `AzureCatalogFilter`. A catalog filter override IS supported at
config init, per catalog URL, via `Configuration::AddCatalogUrl(url,
filter_override)` (C++ `foundry_local_cpp.h:252`; C ABI `foundry_local_c.h:912`,
NULL = no override). Stored in `Configuration.catalog_urls`
(`configuration.h:24-28`). Filter semantics: `nullopt`/NULL = catalog default;
`""` = public models; `"''"` = default the built-in Azure catalog uses
(`kDefaultCatalogFilter`, `azure_model_catalog.h:42`). `CatalogFilter` in
`azure_catalog_models.h` is an internal request-body struct, not public config.

## Related session artifact docs (in this same files/ folder)
- `cpp-core-ssl-explained.md` — deep, no-background-assumed SSL/TLS explainer.
- `android-build-explained.md` — how the Android build works.
