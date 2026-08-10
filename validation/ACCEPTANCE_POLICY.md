# Foundry Local 2.0.0 — Acceptance Policy & Go/No-Go Criteria

This policy is ratified **before** execution so results from every agent are interpreted
consistently. It is the single source of truth for what "go" means.

## Result classes

| class | meaning |
|-------|---------|
| `pass` | Ran and all assertions passed. |
| `fail` | Ran and one or more assertions failed. |
| `blocked` | Could not run (missing hardware, feed not configured, insufficient resources). Not a pass. |
| `waived` | Known issue approved by release owners; requires an issue link, owner, and user-facing note. |
| `n-a` | Not applicable for this platform **per the ratified manifests** (e.g., CUDA on macOS). |
| `skipped` | Not selected for this run, or automation not yet wired on this agent. |

## Cell categories → gating

| category | examples | gating |
|----------|----------|--------|
| **GA-blocking (mandatory)** | install-smoke, pkg-inspect, chat, tool-calling, embeddings, audio-file, vision, web-server, model-mgmt, ep-bootstrap, crosscutting, compat-upgrade | Must be `pass` (or `waived`). No exceptions on CPU paths. |
| **Documented-unsupported** | any `n-a` justified by the manifest | Allowed; recorded with rationale. |
| **Approved-known-issue** | any `waived` | Allowed only with issue link + owner + release note. |
| **Optional/experimental** | chat-large, audio-stream, model-mgmt-fail, soak-resource, integrations, webgpu paths | Informational; failures logged and triaged but do not block GA by default. |

The `blocking` flag on each cell (see `manifests/coverage_cells.json`) marks GA-blocking cells.

## Severity thresholds

- **CPU-path failure on any in-scope SDK → hard GA blocker.**
- **Advertised GPU/NPU acceleration failure** (CUDA, WinML/DirectML, NPU, CoreML/Metal) →
  blocker for that platform's acceleration claim; triage with release owners.
- **`n-a` is only valid against the manifests.** A missing-but-expected model variant or a
  feature that simply doesn't work is a `fail`, never `n-a`.
- Cosmetic / log-only issues → non-blocking; file an issue.

## Evidence requirement (per cell)

Every record must carry: exact command/log path, environment fingerprint (OS/arch/GPU/
driver/CUDA/runtimes), package name + version + resolved source (+ sha256 where captured),
model alias/variant, and the assertion outcomes. The harness records these automatically.

## Rerun rule

- Never validate a locally patched artifact. Any RC fix requires a **fresh `rc2`** published
  to the feed, then a **targeted rerun** of affected cells plus a full install-smoke pass.
- Record package `sha256` / provenance so **GA-candidate artifacts can be proven byte/
  dependency-equivalent** to the validated RC. Repacking or promotion changes invalidate
  prior results for the affected packages.

## Go / No-Go decision

**GO** iff, across the required platform × hardware coverage:
1. Every GA-blocking cell is `pass` or `waived`.
2. No un-waived `fail` or `blocked` remains in a GA-blocking cell.
3. Every `n-a` is justified by the manifests.
4. GA-candidate artifacts are proven equivalent to the validated RC.

Otherwise **NO-GO** until blockers are triaged, fixed (→ new RC), or formally waived.

## Sign-off owners

Named per SDK and per platform before execution (fill in):

| area | owner |
|------|-------|
| C++ SDK | _TBD_ |
| C# SDK | _TBD_ |
| JS SDK | _TBD_ |
| Python SDK | _TBD_ |
| Windows platform | _TBD_ |
| Linux platform | _TBD_ |
| macOS platform | _TBD_ |
| Release lead (final call) | _TBD_ |
