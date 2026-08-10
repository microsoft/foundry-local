# Foundry Local 2.0.0-rc1 — linux-x64 Validation Prompt

Paste the block below into an AI coding session on the `baijumeswani/rel-2.0.0-validation`
branch, running on a linux-x64 machine, to validate the RC packages end to end and contribute
go/no-go cells to the shared checklist.

**Independence note:** this prompt shares method + infrastructure but deliberately withholds the
macOS run's pass/fail verdicts so the Linux session judges independently. Prior findings live in
`validation/FINDINGS.md` and are framed as hypotheses to confirm or refute. If you want an even
more blind run, delete the "read FINDINGS.md" line — but then the session may re-flag the same
sample bugs from scratch (arguably the honest signal you want).

---

```
You are validating the Foundry Local 2.0.0-rc1 release candidate packages END TO END on
THIS machine (linux-x64), to contribute go/no-go cells to a cross-platform release checklist.
Work on the branch `baijumeswani/rel-2.0.0-validation` (based on `releases/rel-2.0.0`).

GOAL
Produce real, reproducible pass/fail evidence for the linux-x64 platform and record it in the
shared harness under `validation/`. A macOS run already exists; DO NOT trust its verdicts —
treat any prior finding as a HYPOTHESIS to independently confirm or refute on Linux, and derive
your own linux-x64 verdict. Note every place Linux diverges from the recorded macOS results.

START BY READING (do not skip)
- validation/README.md, validation/VALIDATION.md (intent + how to run + matrix)
- validation/ACCEPTANCE_POLICY.md (result classes + go/no-go rules)
- validation/FINDINGS.md (prior findings F1–F4, N1–N6 — hypotheses only)
- validation/manifests/*.json (coverage_cells, platform_manifest, sample_map, model_ep_availability)
- validation/scripts/*.py (reproducible direct-API checks — reuse these)

KEY FACTS (verified infra — reuse, don't re-derive)
- The RC packages are on the anonymous ORT-Nightly ADO feed. Transitive deps come from the
  public registries. Install is TWO-SOURCE: fetch the RC package (no-deps) from the ORT feed,
  then install it + its deps from the public registry. Env vars the harness reads:
    FOUNDRY_VALIDATION_PIP_INDEX   = https://pkgs.dev.azure.com/aiinfra/PublicPackages/_packaging/ORT-Nightly/pypi/simple/
    FOUNDRY_VALIDATION_NPM_REGISTRY= https://pkgs.dev.azure.com/aiinfra/PublicPackages/_packaging/ORT-Nightly/npm/registry/
    FOUNDRY_VALIDATION_NUGET_FEED  = https://pkgs.dev.azure.com/aiinfra/PublicPackages/_packaging/ORT-Nightly/nuget/v3/index.json
  Optionally FOUNDRY_VALIDATION_DEPS_PIP_INDEX / _DEPS_NPM_REGISTRY / _DEPS_NUGET_FEED if the
  public defaults (pypi.org, registry.npmjs.org, nuget.org) are blocked on this box — try the
  defaults first.
- Python module import name is `foundry_local_sdk` (both 1.x and 2.0). Package versions:
  python `2.0.0rc1`, C#/C++ `2.0.0-rc1`, JS `2.0.0-rc1`.
- Inference runs IN-PROCESS over the FFI transport — NO Foundry service is needed. Only the
  web-server/vision samples call start_web_service(). Real model downloads work anonymously.
- Samples may be stale (wrong model aliases, deprecated APIs); a sample failure may be a sample
  bug, not an RC defect — triage before concluding.

WHAT TO VALIDATE ON linux-x64
1. Install + package inspection for all four SDKs (cpp, cs, js, python) — verify linux-x64
   RIDs/artifacts, license presence, native .so are packaged and load.
2. Accelerators: exercise CPU always, and CUDA (and any WebGPU/DML/NPU the box exposes) — verify
   EP detection, download_and_register_eps, and REAL inference on each available EP. Confirm the
   detected accelerator set matches platform_manifest for linux-x64 (fix if wrong).
3. Feature cells via the harness (`./run.sh --list` then `./run.sh`): chat, embeddings,
   tool-calling, vision, audio, web-server, integrations — per SDK where a sample exists.
4. Direct-API cells via validation/scripts/ (run each in a venv with the RC installed):
   model_mgmt_check.py, model_mgmt_failure_check.py, crosscutting_check.py,
   soak_resource_check.py, compat_surface_check.py. Confirm they pass on Linux; investigate any
   divergence. For soak, independently characterize memory across load/unload (macOS saw bounded
   RSS growth that plateaus — verify Linux behavior, don't assume).
5. Native-load contract on Linux: check the ORT/GenAI .so discovery and RPATH/LD_LIBRARY_PATH
   behavior (see .github/instructions/ort-loading-contract.instructions.md).
6. C#: confirm the sample's target framework vs the installed .NET runtime; set
   DOTNET_ROLL_FORWARD=Major if needed.

HOW TO RECORD
- Emit results JSON under validation/results/ (harness does this automatically for feature cells;
  for direct-API scripts, add a FINDINGS.md run-log row with pass/fail counts and the script name).
- Regenerate the matrix + checklist:
    python3 orchestrator/plan_matrix.py
    python3 orchestrator/aggregate.py
  (aggregate is most-severe-wins per cell_id; delete superseded fail JSONs if you re-run a cell.)
- Add FINDINGS.md rows/findings for anything new, tagged platform_id linux-x64. State a linux-x64
  go/no-go verdict per ACCEPTANCE_POLICY.md.
- Commit to `baijumeswani/rel-2.0.0-validation` with trailer:
    Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>
  and push.

CONSTRAINTS
- Autopilot: decide and proceed; don't block on questions you can answer by testing.
- Nothing here should be reported as "blocked" merely because it wasn't run on macOS — if it can
  run on this Linux box (CPU/GPU, real downloads, in-process FFI), RUN it. Only genuinely
  off-platform items (other OSes, absent hardware, legacy 1.x service for runtime-upgrade) are
  legitimately deferred.
- Put scratch/temp files in build/output or /tmp, never in the repo tree.

DELIVERABLE
Updated STATUS_MATRIX.md + results/COMBINED_CHECKLIST.md + FINDINGS.md reflecting linux-x64,
committed and pushed, plus a short summary: what passed, what failed (with fresh triage), and
your independent linux-x64 verdict.
```
