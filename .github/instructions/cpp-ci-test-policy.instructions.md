---
description: "Use when adding a new test fixture, modality, or model dependency to the C++ integration test suite, or when debugging unexpected GTEST_SKIP messages in CI."
applyTo: "sdk_v2/cpp/test/**"
---
# C++ Test CI Policy

## No model downloads in CI

The integration test suite must never pull a multi-GB model over the network during a CI run. The gate is centralized in `SharedTestEnv::AcquireModels()` (`sdk_v2/cpp/test/sdk_api/shared_test_env.h`):

- `fl::test::IsRunningInCI()` (defined in `test/internal_api/test_model_cache.h`) returns true when `TF_BUILD=true` (Azure DevOps) or `GITHUB_ACTIONS=true` (GitHub Actions), case-insensitive — mirrors the C# `IsRunningInCI()` helper.
- When in CI **and** `model.IsCached()` is false, the model is left out of `acquired_`. Per-test `SetUp()` then sees the modality accessor return `nullptr` and calls `GTEST_SKIP()`.
- `FOUNDRY_TEST_DATA_DIR` populates the cache. The path is passed to `Configuration::SetModelCacheDir()`, and `LocalModelScanner` finds models by `genai_config.json` + `inference_model.json` regardless of the `{publisher}/` subdirectory layout.

## Authoring rules

- **New modalities** must declare their need via `SharedTestEnv::AcquireModels({Modality::X})` in `SetUpTestSuite()` and check the accessor in `SetUp()` with `GTEST_SKIP()` on null. Do not call `model.Download()` or `model.Load()` directly from a test fixture.
- **Models that don't fit on CI agents** (vision, large embeddings, GPU-only variants) are expected to skip in CI. That is the design, not a bug — fixtures are already structured to handle it cleanly.
- **Do not add a `FOUNDRY_LOCAL_TEST_ALLOW_DOWNLOAD` escape hatch** without an architectural review. Tests that genuinely exercise download behavior (`DISABLED_DownloadFixture`) are gated by GTest's `DISABLED_` prefix and must be opted in explicitly.
- **Do not bleed CI policy into production SDK code.** No `Configuration::SetReadOnlyCache()` or `DownloadManager` mode flags — this is a test-policy decision.

## Dynamic Engine lane

Engine-capable builds compile and run `DynamicEngineChatTest` using the checked-in
`sdk_v2/cpp/test/testdata/tiny-paged-attention` model. The ordinary CMake testdata rule stages the assets, and
`test::GetTestDataPath` locates them. Missing assets fail suite setup; no model environment variables, GPU,
credentials, shared cache, or downloads are needed. The reproducible generator is beside the model, but generation
dependencies are not required to build or run tests.

This small CPU decoder performs causal attention through real paged KV reads and writes using standard ONNX
operators. Its deterministic output is checked against an independent integer reference, including across retained
turns and isolated concurrent requests. It covers SDK dispatch, paging, eviction/replay, cancellation, usage, and
unload, not CUDA kernel correctness or trained-model quality.

The packaging pipeline includes the unconditional `cpp_test_engine` stage from
`.pipelines/v2/templates/stages-test-engine.yml`. It runs on the existing `onnxruntime-Ubuntu2404-AMD-CPU` pool image
without a custom container and uses standard CPU NuGet packages with a test-only Engine-capable GenAI pin.
`FOUNDRY_LOCAL_REQUIRE_DYNAMIC_ENGINE_TESTS=ON` rejects packages that cannot compile the suite. The XML gate rejects
missing lifecycle cases, skipped/disabled/unexecuted tests, and failures. Do not bypass failures with skips or
`continueOnError`. Its binaries are not published as SDK artifacts, and shipping/release dependency pins are unchanged.
Generator-only builds can still exclude the suite; the unconditional Engine lane supplies the required coverage.

## Debugging skips in CI

If model-using tests skip unexpectedly in CI, check the `SharedTestEnv: CI detected` banner in stdout — it reports the value of `FOUNDRY_TEST_DATA_DIR`. `(unset; all model-using tests will skip)` means the CI agent didn't mount the shared model cache. A specific `SharedTestEnv: skipping <model> in CI` line means the cache is mounted but that particular model isn't pre-staged.
