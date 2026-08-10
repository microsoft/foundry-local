# Foundry Local 2.0.0 — Release Validation (intent, matrix, how to run)

> **Purpose.** Thoroughly validate the `2.0.0-rc1` packages end-to-end across platforms so
> we can make a defensible **go/no-go** decision for the public 2.0.0 release. This is the
> durable, shared intent doc: any session/agent that clones `releases/rel-2.0.0` can read
> it, run the same suite on its machine, and report findings back (see
> [`FINDINGS.md`](FINDINGS.md)).

Foundry Local 2.0.0 is a major release: the core is ported from an internal C#
implementation to an **open-sourced C++ runtime** with SDK bindings for C++, C#, JS, and
Python. RC packages are published to the **ORT-Nightly Azure DevOps feed**.

- Acceptance policy & go/no-go rules: [`ACCEPTANCE_POLICY.md`](ACCEPTANCE_POLICY.md)
- Frozen platform baseline: [`manifests/platform_manifest.json`](manifests/platform_manifest.json)
- Model/EP availability (what a variant should exist for): [`manifests/model_ep_availability.json`](manifests/model_ep_availability.json)
- Feature/cell catalog: [`manifests/coverage_cells.json`](manifests/coverage_cells.json)
- Result schema: [`schema/result_record.schema.json`](schema/result_record.schema.json)

## Packages under test (RC, from the ORT-Nightly feed — NOT source builds)

| SDK | Package | Version |
|-----|---------|---------|
| C# | `Microsoft.AI.Foundry.Local` | `2.0.0-rc1` (NuGet) |
| C++ | `Microsoft.AI.Foundry.Local.Runtime` | `2.0.0-rc1` (NuGet) |
| JS | `foundry-local-sdk` | `2.0.0-rc1` (npm) |
| Python | `foundry-local-sdk` | `2.0.0rc1` (PyPI-style) |

Pinned native deps to confirm present: onnxruntime `1.28.0`, onnxruntime-genai `0.15.2`,
windows-ai-machinelearning `2.1.70`.

## Platform × hardware matrix

| Platform | CPU | GPU | NPU |
|----------|-----|-----|-----|
| Windows (x64/arm64) | CPU | CUDA **and** WinML/DirectML | NPU via WinML |
| macOS (Apple Silicon) | CPU | CoreML/Metal | — |
| Linux (x64) | CPU | CUDA | — |

WebGPU is treated as **experimental** (non-blocking). Exact EP per accelerator and the
required/experimental status live in `platform_manifest.json`.

## Feature coverage (representative model slice)

Model management (list/variant-select/download+cancel/cache/offline/update/pin), EP
bootstrapping (download+register, auto + explicit selection, fallback), chat
(non-streaming + streaming), tool calling (native + web-server), embeddings (single +
batch), audio transcription (Whisper file + live/streaming), vision via Responses API,
local OpenAI-compatible web server (+ negative/lifecycle cases), OpenAI-SDK + LangChain
integrations, cross-cutting (cancellation/offline/lifecycle/concurrency/telemetry-privacy),
plus non-functional soak/resource and 1.x→2.0 compat/upgrade.

Representative models: `qwen2.5-0.5b` (chat small), `deepseek-r1-distill-qwen-14b` (chat
large), `qwen3-embedding-0.6b` (embeddings), `whisper-tiny` (audio file),
`nemotron-speech-streaming-en-0.6b` (audio streaming), `Qwen2.5-VL-7B-Instruct` (vision).

---

## How to run the harness (same suite on every machine)

The orchestrator is **stdlib-only Python 3.10+** — no packages needed to run it.

### Fleet quickstart (per machine)

Each agent runs the **same three steps**; only the feed secrets differ per environment. The
harness auto-detects the platform and hardware and runs exactly the cells that machine owns
(see [`STATUS_MATRIX.md`](STATUS_MATRIX.md) for the per-platform scope).

```bash
# 1. Export the feed env vars (see step 1 below for the exact URLs).
# 2. Run every cell applicable to this machine, then aggregate:
./validation/run.sh                                   # Linux/macOS   (pwsh validation/run.ps1 on Windows)
python3 validation/orchestrator/aggregate.py          # roll up + provisional go/no-go verdict
# 3. Commit this machine's results/<hostname>__<run-id>.json and append a row to FINDINGS.md.
```

Owner split (from the frozen platform manifest): **windows-x64** → cpu, cuda, winml-dml,
npu-winml, webgpu · **windows-arm64** → cpu, winml-dml, npu-winml, webgpu · **linux-x64** →
cpu, cuda, webgpu · **macos-arm64** → cpu, coreml-metal. Regenerate the tracking matrix any
time with `python3 validation/orchestrator/plan_matrix.py`.



```bash
# --- RC package source: the ORT-Nightly feed (anonymous for its own packages) ---
# Full URL derivation + auth notes: validation/manifests/feeds.json
export FOUNDRY_VALIDATION_NUGET_FEED="https://pkgs.dev.azure.com/aiinfra/PublicPackages/_packaging/ORT-Nightly/nuget/v3/index.json"   # C# + C++
export FOUNDRY_VALIDATION_NPM_REGISTRY="https://pkgs.dev.azure.com/aiinfra/PublicPackages/_packaging/ORT-Nightly/npm/registry/"        # JS
export FOUNDRY_VALIDATION_PIP_INDEX="https://pkgs.dev.azure.com/aiinfra/PublicPackages/_packaging/ORT-Nightly/pypi/simple/"            # Python
export FOUNDRY_VALIDATION_FEED_TOKEN="<ORT feed PAT; only needed if the feed itself is private>"

# --- Transitive-dependency source (everything that is NOT the RC package) ---
# The ORT feed's upstream proxy needs auth to save an upstream package on first fetch, so
# deps (cffi, node-addon-api, Microsoft.ML.OnnxRuntime, Betalgo.Ranul.OpenAI, ...) come from
# here. Defaults to the PUBLIC registries; override to a mirror if public egress is blocked.
export FOUNDRY_VALIDATION_DEPS_NUGET_FEED="https://api.nuget.org/v3/index.json"     # or a corp mirror
export FOUNDRY_VALIDATION_DEPS_NPM_REGISTRY="https://registry.npmjs.org/"           # or a corp mirror
export FOUNDRY_VALIDATION_DEPS_PIP_INDEX="https://pypi.org/simple"                  # or a corp mirror
```

> Install model: the RC package is fetched from the ORT feed (pip `download --no-deps` /
> `npm pack` / NuGet source-mapped to the two RC package IDs), then transitive deps resolve
> from the deps source only. Validated on macos-arm64: all four RC packages install & load.

The harness writes throwaway `nuget.config` / `.npmrc` and uses isolated NuGet/npm/pip +
model caches inside its run workspace, so your global config is never mutated.

### 2. Inspect what will run on this machine

```bash
# Windows
pwsh validation/run.ps1 -List
# Linux/macOS
./validation/run.sh --list
# or directly:
python3 validation/orchestrator/run_validation.py --list
```

### 3. Run (all applicable cells, or a subset)

```bash
# Everything applicable to this machine:
./validation/run.sh
# A subset:
python3 validation/orchestrator/run_validation.py --sdk python,js --feature install-smoke,chat
# Dry run of the framework without installing anything:
python3 validation/orchestrator/run_validation.py --simulate
```

Selectors: `--sdk cpp,cs,js,python`  `--feature <ids>`  `--accelerator cpu,cuda,winml-dml,npu-winml,coreml-metal,webgpu`.
Unsupported accelerators for the machine are recorded as `n-a` (per manifest) or `blocked`
(supported by the platform but no hardware here) — never a false `fail`.

### 4. Results

Each run writes to `validation/results/`:
- `<hostname>__<run-id>.json` — one schema-valid record per cell (the machine-readable
  artifact that merges across agents).
- `<hostname>__<run-id>.md` — human summary + any blocking failures.

Report them per [`FINDINGS.md`](FINDINGS.md). Aggregate across machines with:

```bash
python3 validation/orchestrator/aggregate.py
```

Exit code `2` from the orchestrator means at least one **GA-blocking cell failed**.

---

## Runner automation status

`install-smoke` is fully automated for all four SDKs (installs the RC into a fresh isolated
project and asserts the reported version is `2.0.0-rc1`). Remaining feature runners are
staged as extension points in `orchestrator/runners.py`: until wired on an agent they emit
`skipped` with a pointer to the manual procedure below — the matrix stays honest and the
harness never crashes. Wiring a feature runner means: prepare an isolated copy of the
corresponding sample, pin the RC package + feed, run it, and assert on output. Contribute
runners back so every agent benefits.

### Manual procedure (until a runner is automated)

For each feature cell not yet automated: install the RC package into a clean project on the
target machine, exercise the feature with the representative model, confirm correct output,
and record a result row in `FINDINGS.md` (or drop a hand-written record into
`validation/results/` following the schema).
