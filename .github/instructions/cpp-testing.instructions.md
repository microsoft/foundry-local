---
description: "Use when writing or modifying C++ integration tests, test helpers, SharedTestEnv, model fixtures, or debugging test failures involving model selection or variant filtering."
applyTo: "sdk_v2/cpp/test/**"
---
# C++ Testing

## Variant Selection in Test Helpers

`GetModels()` returns only the *selected* variant per alias group. The catalog's default sort prefers GPU variants. If a test needs a CPU-only model (e.g., embeddings), the CPU variant is invisible unless you use `GetVariants()`.

Test helpers that filter by device type **must**:
1. Iterate `GetVariants()` on each alias group
2. Call `SelectVariant()` when the currently-selected variant doesn't match the desired device/task
3. Never assume the selected variant matches the filter criteria

See `FindSmallestModelByTask`, `FindSmallestModelByName`, `FindSmallestToolCallingModel` in `shared_test_env.h` for the pattern.

## SharedTestEnv Model Loading

`SharedTestEnv` loads models once for all integration tests. The setup phase:
1. Populates catalog from Azure
2. Downloads CUDA EP (~90% → 100% progress)
3. Loads chat, tool-calling, audio, streaming-audio, and embeddings models
4. Prints `SharedTestEnv: <role>=<model-name>` for each

If a model role prints `(none)`, the test fixtures for that role will skip. Check variant selection logic first — the model likely exists but the wrong variant is selected.

For embedding tests, `FOUNDRY_TEST_EMBEDDING_MODEL` selects an exact model variant ID instead of the default CPU
selection. Combine it with `FOUNDRY_TEST_DATA_DIR` to exercise a pre-cached CPU or GPU model; the CI no-download gate
still applies.

## Test Binary Quick Reference

| Binary | Type | Startup | Count |
|--------|------|---------|-------|
| `foundry_local_tests.exe` | Unit | Fast (no models) | ~700 |
| `sdk_integration_tests.exe` | Integration | Slow (model loading) | ~100 |

Always use `--gtest_filter` when iterating on a specific test or small group. The full integration suite takes ~10 minutes.

## Inner-loop workflow for a single test

When verifying a fix or a new test, never re-run the whole suite via `ctest`. The fast path is:

1. **Incremental build only** (skip configure and the full test phase):
   ```
   python build.py --build --config RelWithDebInfo
   ```
2. **Run the specific test binary directly with `--gtest_filter`**:
   ```
   cd sdk_v2\cpp\build\Windows\RelWithDebInfo\bin\RelWithDebInfo
   .\foundry_local_tests.exe   --gtest_filter="MySuite.MyTest"
   .\sdk_integration_tests.exe --gtest_filter="ModelFixture.TestName"
   ```

Filter syntax: `Suite.Test`, wildcards (`Suite.*`), or colon-separated lists (`Test1:Test2:Test3`). This avoids the ~10-minute model load in `sdk_integration_tests` when iterating on an unrelated test.
