# Model Package Integration into Foundry Local — Scope & Plan

**Status:** Design / scoping (pre-implementation)
**Audience:** Engineer picking up the Foundry Local model-package integration work.

This document is self-contained. It captures what a model package is, what we are
(and are not) building in the first stage, the design decisions and their rationale,
where the work lands in the Foundry Local codebase, open questions, and future
improvements. Read the "References" section for the authoritative format specs.

---

## 1. Background: what a model package is

An ONNX Runtime **model package** is a directory-based model format that bundles
multiple build **variants** of the same model (e.g. builds for different execution
providers (EPs), devices, or compiler flags) behind a single manifest, plus
content-addressed **shared assets** that variants can reference instead of
duplicating. onnxruntime-genai (GenAI) can load a package directly and, at load time,
select the variant that best matches the local hardware.

Key structural facts (see References for full detail):

- A package is a **directory** with a top-level `manifest.json`. A directory is treated
  as a package when it has a top-level `manifest.json` and **no** top-level
  `genai_config.json`.
- A package has one or more **components**; each component has one or more **variants**.
  A variant is always a directory on disk and is otherwise self-contained (its own
  `genai_config.json`, ONNX graph, external weights, etc.).
- **Shared assets** are directories named `shared_assets/sha256-<hex>/`, addressed by a
  content hash. Variants reference them by embedding `sha256:<hex>[/sub/path]` strings
  inside their `executor_info` payload; the executor (e.g. GenAI) resolves those strings
  to real on-disk paths via the model-package library API.
- The manifest's `shared_assets` map can **override** where a given `sha256:<hex>` asset
  lives on disk (including pointing at a location outside the package). This is the
  mechanism that later enables a shared cache without hard links.
- Variant selection uses `ep`, `device`, and an EP-defined opaque `compatibility_string`
  scored by the EP; ORT/GenAI picks the highest-scoring compatible variant.

### Important spec constraint we must design around

The model-package spec **intentionally does not track** which shared-asset directories
exist or which variants consume them. The library has no manifest-level list of
"variant X uses asset Y" — it only resolves `sha256:` strings on demand. Foundry Local,
however, **needs** that mapping to do correct cleanup and cache management. The spec
provides a per-variant free-form `additional_metadata` (and the `executor_info`
extension point) where Foundry Local can record its own bookkeeping fields (e.g. the
list of shared assets a variant depends on). **Stage 1 should populate and rely on such
a Foundry-Local-owned metadata field rather than trying to change the spec.**

---

## 2. Scope of the first stage ("make it work" / crawl)

The guiding principle is **make it work, keep it simple; optimize later.** Stage 1 makes
Foundry Local model-package-aware end to end for the simple, real case we have today, and
explicitly defers weight-sharing optimizations and on-device merging to follow-up work.

### 2.1 In scope

1. **Per-EP package split.** Publish/consume one model package per execution provider
   (e.g. an OpenVINO package, a CUDA package). This is the simplest sensible granularity
   and matches how Foundry Local's catalog already lets a user pick, say, an OpenVINO
   model. We do **not** publish each individual variant as a separate catalog entry.

2. **Single-component, multi-variant packages.** Restrict Stage 1 to the case we
   actually have: a **single EP** package containing **multiple compiled variants** that
   may share weights (via shared assets). Only concern ourselves with the
   current GenAI-compatible packages, which are single-component. Multiple component
   models are a future concern.

3. **Make Foundry Local model-package-aware.** This is the bulk of Stage 1 and touches
   many small places (see Section 4): downloading a package, writing/recognizing the
   local "is this downloaded" marker, scanning the cache at startup to discover
   downloaded packages, loading the package through GenAI, and removing a package.

4. **Version-update download smarts within a single EP package (basic form).** When a new
   **version** of the same EP package is downloaded, avoid re-downloading content that is
   already present locally. Because content is checksum-addressed, we can detect
   shared-asset (checksum) directories already present in the installed previous version,
   **skip downloading them, and copy them from the old version's folder into the new
   version's folder.** New versions are assumed to contain everything the old one had plus
   new variants, so this is a copy-forward, not a merge/unmerge. This keeps everything
   inside the per-EP package folder and avoids introducing a cross-package shared cache in
   Stage 1. (Note: this saves download bandwidth but not disk space — the follow-up crew
   makes sharing "real" so the copy is unnecessary.)

5. **Catalog metadata to mark packages.** The catalog entry needs something that flags an
   entry as a model package (vs. a flat model) and, if selective download is pursued,
   carries the compatibility strings. Scope the minimum needed to download and load; richer
   multi-component metadata can be a follow-up.

6. **Selective-download evaluation (decision, possibly implementation).** Create a real
   OpenVINO package with all its variants, measure its size, and decide whether
   compatibility-string-driven **selective download** (download only the variant(s) that
   fit the local hardware) belongs in Stage 1 or a follow-up. Let the measured size drive
   the decision. (Compiled OpenVINO variants observed on the order of ~1 GB of
   context/compiled data per variant on top of shared weights that can be multiple GB, so
   the savings may be material — measure before deciding.)

### 2.2 Explicitly out of scope for Stage 1 (deferred — see Section 6)

- A **cross-package / cross-alias shared-asset cache** with symlinks/junctions or manifest
  overrides.
- **On-device merging** of separately-downloaded per-EP packages into one multi-variant
  package.
- **Multi-component** packages / pipelines (e.g. stable diffusion), tool-discovery
  components, automatic variant selection built on top of merged packages.
- **Splitting a single EP into multiple catalog entries** (per compiled variant). Keep one
  entry per EP.
- On-device **recompilation** of a model.

---

## 3. Design decisions and rationale

### 3.1 Split granularity = per EP

The catalog is immutable, so packages must be split at publish time at a sensible
granularity. Per-EP is the simplest unit that maps to the existing Foundry Local UX
(the user already picks an EP-specific model). Splitting further (per compiled variant)
adds catalog and merge complexity for little value right now, so we don't.

### 3.2 Package naming, and why on-device merging is deferred

- Catalog models are grouped by **model alias** today. If we published one "uber" package
  with all variants, its package name would simply be the alias. Splitting per EP means we
  may add a variant qualifier (e.g. `alias.variant`), but two per-EP packages that share
  an alias are *implicitly* mergeable.
- **On-device merging** (combining separately downloaded per-EP packages into one
  multi-variant package so you can switch/choose among EPs) is desirable in the future but
  **not** required now. It introduces hard problems we chose not to solve yet:
  - *Validity of a merge:* two packages should only merge if they come from the **same base
    model**. This needs some identifier (e.g. a base-model name/checksum recorded in
    metadata) to confirm compatibility before merging.
  - *Metadata compatibility:* differing authoring-tool versions (e.g. OLive v2 vs v3) raised
    the question of what blocks a merge. Working conclusion: as long as the **model-package
    schema version** matches and each still produces a loadable model, packages should be
    mergeable; the authoring-tool version likely does not matter.
  - *Filesystem reconciliation:* if package A and B merge into one, do you keep A and B or
    fold their directories together? Then "what was downloaded" diverges from "what's on
    disk," complicating bookkeeping.
- Because today's different-EP variants generally do **not** share weights (they come from
  different conversion paths), merging different EPs yields little benefit now. So it stays
  a future item.

### 3.3 Shared-asset bookkeeping via Foundry-Local metadata

The executor (e.g. GenAI) owns resolving `sha256:` references to disk paths — the package
itself doesn't know how the executor consumes shared data. But Foundry Local must know
which shared assets each variant uses to:
- clean up shared-asset directories no longer referenced by any remaining variant after a
  variant/package is deleted, and
- decide what can be skipped on download.

Since the spec won't enforce this mapping, Foundry Local records the assets a variant uses
in a Foundry-Local-owned metadata field (per-variant `additional_metadata` /
`executor_info` slot). Deletion then becomes: remove the variant's directory and its
metadata entry, then iterate remaining variants to see which shared assets are still
referenced and delete the orphans.

### 3.4 Simplicity over disk savings, generally

Weight sharing across variants is, in practice, an **edge case** today: the main cases are
CUDA + generic GPU, and Intel NPU "compound" variants. A developer typically uses one
variant (Foundry Local picks the best), so avoiding shared-weight downloads is nice-to-have
marketing more than a real user win right now. Therefore Stage 1 optimizes for simplicity;
complexity can always be added later.

### 3.5 Why the model package abstraction matters (the longer-term "why")

Weight sharing is the minor benefit. The larger value is enabling scenarios beyond
"run one model": model pipelines (e.g. stable diffusion), built-in tool
calling / tool-discovery components (including MCP-server discovery) so app developers get
tools without building the tool-calling infrastructure themselves, image pre/post-
processing, and automatic variant selection. Stage 1 is deliberately the "crawl" step that
puts the infrastructure in place so these can be built out later.

---

## 4. Working with the Foundry Local repo — high-level guidance

The implementing engineer should map the exact files themselves. This section only captures
the high-level topics and tips the Foundry Local tech lead shared about doing this work, so
you know what to expect and what to watch for:

- **This is a "make it work" effort with broad but shallow surface area.** None of the
  individual changes are especially complex, but making Foundry Local model-package-aware
  touches **a lot of little places** — download, local-cache discovery, load, and removal all
  need to learn about packages. Budget for breadth, not depth.
- **"Downloaded" marker.** Today Foundry Local writes/looks for an `inference_model.json`
  file that effectively acts as the flag for "this model is downloaded." A package may hold
  multiple components/variants, so decide early whether that same marker still works or
  whether a package-level marker is needed.
- **Startup cache discovery.** On startup Foundry Local iterates the model cache directory to
  discover which models are already downloaded. This logic must become package-aware
  (recognize a package layout and resolve the correct model id) rather than assuming the
  current flat-model layout.
- **Per-version folders.** Downloaded content lands in a folder suffixed by the model
  version. This is what makes the Stage-1 copy-forward optimization (copy already-present
  checksum/shared-asset directories from the previous version's folder) straightforward.
- **Download and removal both need attention.** For removal in particular, work out how to
  know the *right* things to delete for a package, and whether any special handling is needed
  on the storage/BLOB side.
- **Foundry Local is where future pipelining logic will live**, so it must be package-aware
  even though Stage-1 usage is simplistic. Keep the downloading logic simple in v1 and push
  weight-sharing optimizations to a follow-up crew.
- **Catalog changes.** The catalog entry needs extra metadata to mark an entry as a model
  package and (if selective download is pursued) to carry compatibility strings up front. How
  to represent multiple component models is still open. Note there is an in-progress migration
  to **catalog v2**, and a **private catalog** with a test instance that may help testing.
- **General design advice from the tech lead:** if you find yourself fighting the design,
  something is probably off with it; keep asking "what do we *need* vs. what's *nice to
  have*"; keep it as simple as possible for now — complexity is easy to add later.

---

## 5. Testing & catalog access

- No confirmed public test catalog for this. Options, in order of preference:
  1. Ask about an **integration test catalog** and/or the **private catalog test instance**
     (it should be Foundry-Local-compatible; Foundry Local can talk to it to find a model,
     and you can control its BLOB storage to replace content freely — useful because
     published catalog entries are immutable, though you can add new versions).
  2. Fall back to a **fake catalog endpoint** that returns the JSON Foundry Local expects,
     pointed at your own BLOB/storage, to exercise the download path.
- Be careful publishing test models to the official catalog: entries are immutable (can't
  edit, only add a version), though test models are now filtered from public view.
- **Follow-up owners noted:** confirm test/private-catalog access with the catalog owners
  (Sunghoon for the integration test catalog; the private-catalog team, e.g. via Emmanuel).

---

## 6. Future improvements (explicitly deferred)

These are intentionally **not** in Stage 1. Design Stage 1 so they remain possible.

1. **Shared-weights optimization as a dedicated follow-up feature crew.** Promote sharing
   from "copy-forward within a version folder" to a real shared store, so no copy is needed.
   This crew bundles: the smart/skip download, a shared-data cache folder, and the logic to
   know when a shared asset is referenced by no remaining variant. A natural cache layout is
   a `shared_data` folder under the models cache, sub-grouped by **model alias** for
   traceability, holding the `sha256-<hex>` directories; on download, check presence there
   before fetching. The manifest's `shared_assets` override already lets a variant point at
   such an out-of-package location **without** hard links/junctions, so this is feasible
   within the format. (Catalog BLOBs must stay self-contained — Azure BLOB storage can't
   reference another BLOB — but that's fine: we only need to reduce **download bandwidth and
   local disk**, not cloud dedup.)

2. **A cross-package / cross-alias central shared-asset cache.** Because assets are
   content-addressed, even different EP packages (or different aliases) could in principle
   share identical assets from one central location. We have **no** current use case where
   different EP packages have overlapping assets, so this is speculative — but the
   content-addressed design leaves the door open. Don't build it until a real case appears.

3. **On-device merging** of per-EP packages into one multi-variant package (Section 3.2),
   enabling EP/device switching and more advanced selection. Requires the merge-validity
   check (same base model), schema-version compatibility check, and a filesystem
   reconciliation strategy.

4. **On-device recompilation.** A future need is recompiling a model on device (e.g. a new
   CUDA/driver version, or an IHV wanting to recompile in place). The good news is that the
   components required for this are largely already present in a package: for the EPs doing
   per-EP compiled variants today (OpenVINO), loading the compiled variants already requires
   the base weights and typically the base (non-compiled) ONNX model, so those artifacts are
   in the package anyway (commonly as shared assets). So this is expected to be mostly a
   matter of extending the **model-package logic** to expose/consume those artifacts for
   recompilation, rather than changing what Foundry Local ships. If a case ever needs the
   base model where it isn't already present, prefer adding it in the compile/authoring step
   (e.g. publishing a new package version that includes it) over Foundry Local synthesizing
   it. Just ensure the Stage-1 design doesn't block this later.

5. **Multi-component packages / scenario pipelines**, **tool-discovery / MCP components**,
   **image pre/post-processing components**, and **automatic variant selection** built on the
   package abstraction (Section 3.5). Catalog metadata will need to represent multiple
   component models and their compatibility data types to support these.

6. **Selective download by compatibility string** (if not pulled into Stage 1 after the size
   measurement) — download only the variant(s) compatible with the local hardware instead of
   the whole package. Requires the catalog entry to carry compatibility strings up front.

---

## 7. Suggested Stage-1 work breakdown (starting checklist)

1. Make Foundry Local **load** a model package through GenAI (integrate the model-package
   library / GenAI package loading; single-component, per-EP).
2. Make the **startup cache discovery** package-aware: recognize a downloaded package
   layout, resolve the correct model id, understand variant subdirectories, and decide the
   "downloaded" marker for packages.
3. Make **download** write the correct package layout + marker, including per-version folders.
4. Make **removal** package-aware, including variant directories (and, later, shared assets).
5. Add **catalog metadata** to mark an entry as a model package; coordinate with catalog v2.
6. Implement **version-update copy-forward** of already-present checksum/shared-asset
   directories from the previous version folder (basic download smarts).
7. Add the **Foundry-Local shared-asset bookkeeping** metadata field per variant and use it
   for cleanup.
8. **Create a real OpenVINO package**, measure size, and decide selective-download placement.
9. Stand up a **test catalog** path (private/integration catalog, or fake endpoint).
10. List remaining items and decide the **shared-weights optimization feature-crew** cut line.

---

## 8. References (authoritative specs)

- Model-package library (format, manifest/component schema, shared assets, path resolution,
  authoring API, `executor_info` extension point):
  https://github.com/microsoft/onnxruntime/blob/main/model_package/README.md
- ORT consumer-side integration (`executor_info["ort"]` schema, variant selection algorithm,
  compatibility-string scoring, session creation, experimental C API):
  https://github.com/microsoft/onnxruntime/blob/main/onnxruntime/core/session/model_package/README.md
- onnxruntime-genai model-package loading (package detection, `sha256:` tokenizer sharing,
  `from_package_ep`, C/C++/Python load APIs):
  https://github.com/microsoft/onnxruntime-genai/blob/main/docs/model_package.md
