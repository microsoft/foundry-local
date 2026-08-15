# Multi-Catalog Support — Consolidated Plan

> Status: **Proposal for review**
> Scope: `sdk_v2/cpp` catalog subsystem
> Supersedes: `MultiCatalogAggregationPlan.md` and `MultiSourceCatalogDesign.md`
> (kept for history; do not edit)

## Summary

Refactor the catalog subsystem so multiple catalog types — **Public** (Azure) and, as future
follow-ups, **Private** (an additional online catalog source) and **BYOM local** — feed **one
aggregated `ICatalog`**. The public API (`ICatalog` / `flCatalogApi`) stays byte-for-byte identical;
the multiple contributing catalogs are an implementation detail.

**Initial scope is the Public (Azure) source only.** Locally-cached models are still surfaced, but
**not via a separate source**: the Azure source keeps today's behavior of scanning the cache, folding
local ids into its live fetch, and synthesizing stub metadata for disk-only ("BYOM") models. The only
new local concept is the **`kLocal`** tag on those synthesized orphan stubs (see *What "local" means*
below), a short-term marker removed when BYOM lands. The **Private** and **BYOM local** catalogs are
deferred; the source/store split keeps the store source-agnostic so each lands later as a new source,
not a redesign.

Split the two responsibilities that `BaseModelCatalog` currently couples:

- **Fetch** moves into a new `IModelSource` interface (one per catalog type). Sources are
  **pure fetchers that return `ModelInfo`** — they do not create `Model` instances.
- **Store / query / index / create** lives in a single `ModelCatalog : ICatalog` that owns the
  `ModelFactory` and all `Model` instances, merges across sources, keeps shadow duplicates, and
  serves a filtered (preferred-only) public API view.

On a duplicate (same `model_id` across catalog sources) we keep every copy internally as **shadow
variants**; the **public API view** surfaces only the **preferred** one. Preference is by catalog
source: `local > private > public`. Retaining the shadows enables **fallback** — e.g. unregistering
a BYOM local model that shadows a cloud id re-selects the surviving cloud copy. (Genuine cross-source
duplicates only arise once Private or BYOM lands — see *What "local" means* below — so this machinery
is foundational in the initial scope.)

## What "local" means here

`ScanLocalModels` (the existing cache scanner, `catalog/local_model_scanner.{h,cc}`) returns
`model_id → local_path` for every model present in the cache dir. The Azure source folds those ids
into its fetch, and they split into two buckets — **only the second is "local":**

- **Cached public model — *not* local.** The scanned `model_id` resolves against an online catalog
  (public latest, or public by-id for an older version). This is a **Public** model with local state
  attached: `catalog_source` stays `kPublic`; the fetch marks it cached and sets `local_path`. It
  stays a single leaf, not a duplicate.
- **Local stub (`kLocal`) — short-term only.** The scanned `model_id` matches **nothing** in any
  online catalog. The Azure catalog synthesizes thin **stub metadata** (`MakeByomModelInfo` →
  `AddLocalModels`) so the id is a valid catalog entry, which we tag **`kLocal`**. These
  orphan stubs are the *only* genuinely-local entries today.

The dedicated **BYOM local catalog** (future follow-up) **replaces this stub-based `kLocal`
support** with first-class local models; the short-term stub path exists only until then. No separate
`LocalModelSource` is introduced now — it would add a merge/ownership split with no behavioral gain
while there are no genuine shadows.

Consequence for the initial scope: public ids are unique and `kLocal` stubs are orphans (disjoint
from public by construction), so **the single Public source produces no genuine cross-source
duplicates**. The shadow-variant / source-preference / `UniqueVariants` machinery is therefore
foundational — first exercised when Private or BYOM introduces a second source that can serve the same
`model_id`.

## Why this shape

- **Source/store split; sources return `ModelInfo`.** The store owns the `ModelFactory`, so sources
  stay pure fetchers and never touch `DownloadManager` / `ModelLoadManager`.
- **Duplicates are shadow variants, filtered by the public view.** There is no internal
  "no duplicates" rule — the user drives selection, so the visible `flModelList` is just a filtered
  view, and same-`model_id` copies from different sources may carry different metadata.

Two behavior changes fall out of this:

1. **Container `variants_` may hold same-`model_id` shadows** (today they're distinct). Filtering
   moves to the visible view; the internal **`id_index`** (the `model_id → Model*` lookup that backs
   by-id queries, built by `RebuildIndex`) resolves each `model_id` to its **preferred** leaf via
   source-aware ordering.
2. **`Model*` stays stable for cloud models** (append-only; `RemoveFromCache` only un-caches). The
   one exception is a **BYOM local `Unregister`** (future) — an explicit, user-initiated removal.

## Confirmed decisions

| # | Topic | Decision | Rationale |
|---|---|---|---|
| D1 | Architecture | **Source/store split.** One owning store; sources are fetch-only. | Preserves `Model*` stability and single ownership; reuses existing index/refresh. |
| D2 | Sources return | **`ModelInfo`, not `Model`.** The store owns the `ModelFactory`. | Keeps sources free of `DownloadManager` / `ModelLoadManager` coupling. |
| D3 | Catalog source | **Explicit `CatalogSource` enum field on `ModelInfo`**, not a property-bag entry, and **not** an overload of `model_provider`. | It is correctness-critical (drives dedup/preference), sits on the compare hot path, and is a small closed set. `model_provider` describes *who publishes*; catalog source describes *which catalog served* it. |
| D4 | Duplicate storage | **Shadow variants** inside the alias container; the public API view filters to the preferred copy. | Internal duplicates are harmless (the user controls model selection); the visible list is a filterable `flModelList` view. Keeps each source's full metadata for fallback. |
| D5 | Preference | `local > private > public`, applied by **source-aware variant ordering** so both by-`model_id` lookup and the visible view resolve to the preferred copy. | Small, closed tiebreak; applies only when two sources serve the same `model_id` (the unique model identifier). |
| D6 | Cached-state / non-latest metadata | The local scan attaches `local_path` + cached state to the matching catalog entry. For cached **non-latest** versions absent from the latest cloud fetch, resolve full metadata via the online source's `FetchModelsByIds`. | A downloaded cloud model is not a duplicate — it is the cloud entry with local state attached. Matches current behavior. |
| D7 | Private catalog | A **future follow-up**; an additional online catalog source. Its shape, auth, and fetch implementation are **deferred** — the source/store split leaves room for it without a redesign. | Out of initial scope; captured only as a placeholder. |
| D8 | Local models | **No separate source now.** The Azure source keeps today's flow — scan the cache (`ScanLocalModels`), fetch live metadata resolving cached ids by-id (`FetchAllModelInfosWithCachedModels`), then `AddLocalModels` attaches `local_path` and synthesizes stubs (`MakeByomModelInfo`) for disk-only models. The only change: **tag those synthesized stubs `kLocal`**. The dedicated **BYOM local catalog** replaces this stub path later. | Least risk; reuses working code. A separate `LocalModelSource` adds a merge/ownership split with no gain while there are no shadows. |
| D9 | Removal / lifecycle | **Cloud (`kPublic`)** model: `RemoveFromCache` deletes the local dir but keeps the `Model` (re-downloadable). **BYOM local** model *(future)*: `Unregister` removes the `Model`; `RemoveFromCache` is invalid for it. Short-term `kLocal` orphan stubs have no explicit lifecycle API — they drop out on the next refresh when their cache dir is gone. | Distinct semantics; the future BYOM catalog exposes explicit `Register` / `Unregister`. |
| D10 | Delivery | **Initial scope is the Public (Azure) source only** (with today's inline local-cache resolution + the new `kLocal` tag). The Private catalog and dedicated BYOM local catalog are future follow-ups (each an independent source). | Shrinks the initial diff — the store is source-agnostic, so Private and BYOM are additions, not redesigns. |

## Current architecture (verified)

- **`ICatalog`** ([src/catalog.h](../src/catalog.h)) is the query surface; `flCatalog` / `flManager`
  wrap `fl::ICatalog&`, so any implementation keeps the C ABI and consumers unchanged.
- **`BaseModelCatalog`** ([base_model_catalog.h](../src/catalog/base_model_catalog.h)) owns `models_`
  (stable `unique_ptr`) with pure-virtual `FetchModels` / `FetchModelVersions` / `FetchModelsByIds`.
  `PopulateModels` groups leaves by alias; `IntegrateVariants` dedups by `model_id`; `RebuildIndex`
  builds `id_index` / `alias_index` / `name_index` (first-wins).
- **`AzureModelCatalog`** is the only subclass. `FetchModels` scans the cache
  (`ScanLocalModels`), then `GetLiveCatalogOrLocalSnapshot` fetches live metadata across the catalog
  URLs (each via `FetchAllModelInfosWithCachedModels` — fetch latest + resolve cached non-latest
  by-id — deduplicated by `model_id`), falling back to the `CatalogCache` / `foundry.modelinfo.json`
  snapshot when every live URL fails. `AddLocalModels` then attaches `local_path` to matched infos and
  synthesizes stub metadata (`MakeByomModelInfo`) for disk-only models, building leaves via
  `model_factory_`. A `CreateCatalogClient` virtual seam exists for test injection.
- **`GetCachedModels`** ([base_model_catalog.cc](../src/catalog/base_model_catalog.cc)) now iterates
  each container's `Variants()` and returns every cached leaf (not the alias container).
- **`Model`** ([src/model.h](../src/model.h)) is a leaf or a container owning `variants_` +
  `selected_variant_`. `CompareBestFirst` orders device asc, version desc, created-at desc,
  `model_id` asc. `RemoveFromCache` only un-caches.

## Target architecture

```
                    ┌──────────────────────────────┐
   public API  ───> │  ModelCatalog : ICatalog     │  owns ModelFactory, containers,
   / C ABI          │  - models_ / indices / cache │  indices, caching, refresh;
                    │  - vector<unique_ptr<        │  shadow variants, view filters
                    │        IModelSource>> sources│  to preferred
                    └──────────────┬───────────────┘
                                   │ composes (fetch-only; return ModelInfo)
             ┌─────────────────────┼─────────────────────┐
             ▼                     ▼                     ▼
   ┌────────────────────────────┐ ┌───────────────────┐ ┌───────────────────┐
   │ AzureModelSource (kPublic) │ │ Private source    │ │ BYOM Local source │
   │  + inline local-cache      │ │ (kPrivate)*future*│ │ (kLocal) *future* │
   │    resolve: kPublic cached │ └───────────────────┘ └───────────────────┘
   │    / kLocal stub           │
   └────────────────────────────┘
    IModelSource: FetchModels / FetchModelsByIds / FetchModelVersions  → ModelInfo
   (The future BYOM Local source replaces AzureModelSource's short-term inline kLocal stubs.)
```

- The catalog **has** sources; it **is not** a source.
- Each source stamps `info.catalog_source` on every `ModelInfo` it produces. Short-term the Azure
  source stamps `kPublic` on catalog entries (including cached ones with `local_path` attached) and
  `kLocal` on the orphan stubs it synthesizes from the local cache scan.
- The store creates a leaf via its owned `ModelFactory` for every `ModelInfo` (including
  same-`model_id` shadows), groups them by alias into containers, and orders variants source-aware
  so the preferred copy wins in `id_index` and the visible view.

## Design

### New / changed types

- **`CatalogSource { kPublic = 0, kPrivate = 1, kLocal = 2 }`** + `CatalogSourcePriority(src)`
  (local ranks most-preferred). `kPublic = 0` so zero-initialized / legacy `ModelInfo` decodes as
  public. Preference is expressed by the priority helper, independent of the enum's value order.
  The priority helper feeds `CompareBestFirst` as its **final tiebreak** (see below), so it only ever
  reorders genuine duplicates and never perturbs the existing device/version/created-at/`model_id`
  ordering of non-duplicates.
- **`ModelInfo::catalog_source`** — new field, default `kPublic`; round-tripped in
  `ModelInfoFromJson` / `ModelInfoToJson` so it survives the on-disk cache (absent → `kPublic`).
- **`IModelSource`** (new, `src/catalog/model_source.h`), fetch-only, returns `ModelInfo`:
  - `CatalogSource Source() const`
  - `std::string Name() const`
  - `std::vector<ModelInfo> FetchModels() const` (latest, stamped with `Source()`)
  - `std::vector<ModelInfo> FetchModelsByIds(const std::vector<std::string>& ids) const` (default `{}`)
  - `std::vector<ModelInfo> FetchModelVersions(const std::string& alias, const std::string& name = "") const` (default `{}`)
- **`AzureModelSource : IModelSource`** — the fetch guts moved out of `AzureModelCatalog`, serving
  the **Public** catalog. Parameterized by URLs + filter, region, and fallback; reuses
  `MakeCatalogClient` / `AzureCatalogClient` and the `CreateCatalogClient` test seam.
  **Short-term it also owns local-cache resolution** (today's flow): `ScanLocalModels` →
  `GetLiveCatalogOrLocalSnapshot` (live fetch, else `CatalogCache` snapshot fallback) →
  `AddLocalModels`, stamping `kPublic` on catalog entries and **`kLocal`** on the synthesized
  (`MakeByomModelInfo`) orphan stubs, and conveying each cached entry's `local_path` so the store
  marks it cached. This local handling moves to the BYOM source later. (A future Private source is a
  separate follow-up; its shape is TBD.)
- **`ModelCatalog : ICatalog`** — evolves `BaseModelCatalog`. Holds
  `std::vector<std::unique_ptr<IModelSource>> sources_`, the `ModelFactory`, and the existing
  store/indices/refresh. Containers may hold same-`model_id` shadow variants; variant ordering is
  **source-aware** (preferred first) so `id_index` first-wins and the visible view resolve to the
  preferred copy. Adds a local-BYO `Unregister` path that removes a variant (see Removal & fallback).

### Variant ordering, selection & visibility (shadow variants)

Shadow variants (same `model_id` from different catalog sources) live inside the alias container's
`variants_` alongside genuine distinct variants. Three mechanisms keep them internally complete while
the public surface stays de-duplicated:

- **Ordering — `CompareBestFirst` gains a final tiebreak.** The comparator keeps its existing keys
  (device priority asc, version desc, created-at desc, `model_id` asc) and appends
  `CatalogSourcePriority(catalog_source)` **asc** as the *last* key. Genuine duplicates match on all
  four prior keys, so the source key alone decides their relative order (`local > private > public`);
  non-duplicates differ earlier and are unaffected. `AddVariant`'s `upper_bound` insert therefore
  places every shadow in preferred-first order regardless of source insertion order.

- **Internal enumeration — `Variants()` is unchanged and all-inclusive.** It returns every leaf,
  including shadows, and remains the accessor used by internal machinery (`RebuildIndex`,
  `GetCachedModels` / `GetLoadedModels`, merge/integration). Because `variants_` is preferred-first,
  `RebuildIndex`'s first-wins `id_index[model_id]` resolves to the **preferred** leaf automatically.

- **Public enumeration — new `Model::UniqueVariants()`.** Returns `std::vector<Model*>` filtered to
  one leaf per `model_id`: it walks `variants_` in its existing best-first order (under a single lock)
  and keeps the first occurrence of each `model_id` (which, per the tiebreak above, is the
  preferred-source copy), skipping later shadows. This is the accessor **both public surfaces** use:
  the C API `Model_GetVariantsImpl` (`c_api.cc`) and the REST `GET /v1/models`
  `OpenAIListModelsHandler` (`service/models_handlers.cc`) switch from `Variants()` to
  `UniqueVariants()`, so neither emits duplicate `model_id`s. The visible `flModelList` / model list is
  thus a filtered projection; internal storage keeps the full shadow set for fallback.

  *Why a method, not inline filtering:* it has **two** public callers, so centralizing keeps the
  shadow-dedup policy next to the ordering rules rather than duplicated across handlers. It is also
  **allocation-neutral** — these sites already call `Variants()` (a heap snapshot vector) and copy
  into their output; `UniqueVariants()` returns the same single snapshot under one lock. (A
  zero-intermediate callback enumerator was rejected: catalog access is explicitly never
  performance-critical.) Internal callers — `RebuildIndex`, `IntegrateVariants`, `GetCachedModels` /
  `GetLoadedModels` — keep using the all-inclusive `Variants()`.

**Default variant selection (`SelectDefaultVariant`) — cached wins.** The existing rule stands:
select the first **cached** variant in best-first order, else `variants_.front()`. Precedence is
therefore *cached-first, then source-preference among equals*:

- Any **local** model (BYO or a downloaded copy) is locally available, so its leaf is constructed with
  its **cached flag set by default**. A local shadow is thus cached and, being highest source
  priority, is both first-in-order and cached → selected.
- If a lower-priority source's copy is cached but the preferred-source copy is not (e.g. cached
  `public` vs. uncached `private`), the **cached** copy is selected — cached beats source preference,
  by design.

Note a deliberate divergence for shadowed `model_id`s: `id_index[model_id]` (used by
`GetModelVariant`) resolves to the *preferred-source* leaf via first-wins ordering, while the
container's *default selection* may be a *cached* lower-priority leaf. These answer different
questions ("give me the preferred copy of this id" vs. "what does this container act on by default")
and are intentionally allowed to differ.

### Merge algorithm (`ModelCatalog::Populate`)

`Populate` keeps its current shape — cache-only mode, fetch, leaf-build via `ModelFactory`, group by
alias (`PopulateModels`), and `RebuildIndex`. The **one change**: gather `ModelInfo` from all sources
(each stamped by `Source()`) and allow same-`model_id` **shadows** — the dedup key in
`IntegrateVariants` becomes `(model_id, catalog_source)` and variant ordering is source-aware
(`local > private > public`) so `id_index` first-wins and the visible view resolve to the preferred
copy. In the initial single-source scope no shadows arise (see *What "local" means*), so this is
dormant foundation.

### Removal & fallback

Removal is **foundation, first exercised by BYOM (future)** — the initial single-source scope has no
shadows to fall back to. Semantics (per D9): **cloud (`kPublic`)** uses `RemoveFromCache` (un-caches,
keeps the re-downloadable `Model`, removes no variant — unchanged); short-term **`kLocal`** stubs have
no removal API and drop out on refresh once their cache dir is gone; **BYOM local (future)** adds
`Unregister`, which removes the variant and re-selects the surviving cloud shadow (if any). The path,
specified now:

**`Model::RemoveVariant(const Model& variant)`** — under `state_mutex_`: find the matching
`unique_ptr` by address (throw `FOUNDRY_LOCAL_ERROR_INTERNAL` if absent); **erase and compact** (no
`nullptr` holes — every `variants_` walker assumes dense, non-null entries; erase preserves best-first
order); if the removed leaf was `selected_variant_`, re-run `SelectDefaultVariant` (cached-first, then
preferred source), or leave it null if `variants_` is now empty.

**`ModelCatalog::Unregister(model_id)`** — under the catalog mutex: locate the container via
`alias_index`, call `RemoveVariant`, erase the container from `models_` if it became empty, then
`RebuildIndex`.

**Invariant & concurrency.** This is the one place the historically **append-only** `models_` /
`variants_` invariant is relaxed. Removal is a rare, explicit, user-initiated admin op, so — rather
than adding tombstones or generational handles — we accept: it **invalidates outstanding
`flModelList` / `Model*`** obtained before the call (the erased leaf is freed; survivors keep their
addresses because `variants_` holds `unique_ptr`); it is **not safe to call concurrently** with
enumeration or model ops on the affected alias (quiesce first); normal refresh stays append-only and
concurrency-safe. Header "never removed" promises on `base_model_catalog.h` / `model.h` are updated to
carve out this exception.

### Wiring

- **`Manager::Create`** ([src/manager.cc](../src/manager.cc)): build the source list `[Public]`
  (the `AzureModelSource`, which also does the inline local-cache resolution), construct the
  `ModelFactory`, and construct one `ModelCatalog`. `catalog_` stays `std::unique_ptr<ICatalog>`. The
  list is source-agnostic, so future Private / BYOM sources slot in without store changes.

## Delivery phases

Phase 0–3 are the **initial scope: the Public (Azure) source** (with inline local-cache resolution +
the `kLocal` tag). The **Private catalog** and the dedicated **BYOM local catalog** are **future
follow-ups** — each an independent source that needs no redesign of the store, sources, or public API.

### Phase 0 — Types & metadata
1. Add `CatalogSource` enum + `CatalogSourcePriority` helper.
2. Add `CatalogSource catalog_source` to `ModelInfo`; round-trip in
   `ModelInfoFromJson` / `ModelInfoToJson`.

### Phase 1 — Source abstraction *(parallel after Phase 0)*
3. New `IModelSource` interface (returns `ModelInfo`).
4. `AzureModelSource` (fetch guts from `AzureModelCatalog`; serves Public and retains the inline
   local-cache resolution — `ScanLocalModels` + `GetLiveCatalogOrLocalSnapshot` + `AddLocalModels`,
   incl. the snapshot fallback and `CreateCatalogClient` seam).
5. Tag the synthesized orphan stubs `kLocal` in `MakeByomModelInfo` (`azure_model_catalog.cc`) — the
   only local-specific change.

### Phase 2 — Aggregating store *(depends on Phase 1)*
6. `ModelCatalog : ICatalog` — owns `ModelFactory`, `sources_`; reuse group/index/refresh with
   source-aware variant ordering, `UniqueVariants()`-backed preferred-only public view, and the
   `Unregister` / `RemoveVariant` removal path (foundation; dormant with one source).
7. Implement the merge / leaf-build / shadow-duplicate algorithm above; preserve cache-only mode
   and `CatalogCache` save.

### Phase 3 — Wiring *(depends on Phase 2)*
8. `Manager::Create` builds the `[Public]` source, constructs the factory and `ModelCatalog`.

### Phase 4 — Tests *(parallel with Phases 2–3)*
9. `FakeModelSource` helper (replaces the `TestCatalog` `FetchModels` override; returns `ModelInfo`).
10. `model_catalog_test.cc`: **local classification** (scanned id matching public → `kPublic` cached
    single leaf; orphan id → `kLocal` stub) / preference (`local > private > public`) / shadow-variant
    visibility / preferred-only visible view (`UniqueVariants` de-dup) / `SelectDefaultVariant`
    precedence (cached-beats-source; local-cached-by-default) / cached-state attachment /
    cached-non-latest cloud resolution / fallback-on-unregister via `FakeModelSource` shadows
    (`RemoveVariant` compaction + empty-container removal + surviving-shadow re-selection) /
    `catalog_source` round-trips through the cache JSON / union-of-aliases across sources.
11. Migrate the Azure catalog tests (`azure_catalog_test.cc` + `azure_model_catalog_test.cc`, incl. the
    snapshot-fallback / BYOM-synthesis / dedup cases) → `azure_model_source_test.cc` (fetch guts
    unchanged). Keep `catalog_cache_test`, `model_sorting_test`, and `sdk_api/catalog_test` (surface
    unchanged).

## Future follow-ups *(out of initial scope; no redesign required)*

Each is independent and depends only on Phase 3. All slot into the same source/store design.

### Private catalog
- An additional online catalog source (its own `IModelSource`, stamped `kPrivate`), plus any
  `Configuration` and C-API surface it needs. Shape, auth, and fetch implementation are **TBD** and
  designed when the follow-up is scheduled.

### BYOM local catalog
- A dedicated local source (its own **TBD** scan path + first-class local metadata) and a public
  `Register` / `Unregister` API. It **replaces** the Azure source's short-term inline local-cache
  resolution and `kLocal` stubs; `Unregister` wires to the `ModelCatalog` variant-removal path.

### Public visibility of duplicate sources
- Once a real scenario needs it, add a public way for a consumer to enumerate all of a model's
  duplicate catalog sources (the shadows the visible view hides). Not built now — the visible view
  surfaces only the preferred copy, which is sufficient until a concrete need arises.

## Affected files

**New**
- `catalog/model_source.h` (`IModelSource`)
- `catalog/azure_model_source.{h,cc}`
- `catalog/model_catalog.{h,cc}`

**Modify**
- [src/model_info.h](../src/model_info.h) / [src/model_info.cc](../src/model_info.cc) —
  `CatalogSource` enum, `CatalogSourcePriority`, `catalog_source` field + JSON round-trip
- [src/model.h](../src/model.h) / [src/model.cc](../src/model.cc) — source-aware **final tiebreak**
  in `CompareBestFirst` (orders genuine duplicates preferred-first, non-duplicates unchanged); new
  `UniqueVariants()` preferred-only view for the public list (leaving `Variants()` all-inclusive for
  internal use); `RemoveVariant` for local-BYO `Unregister`; local leaves constructed cached-by-default
- [src/catalog/azure_model_catalog.cc](../src/catalog/azure_model_catalog.cc) — `MakeByomModelInfo`
  (the disk-only stub synthesizer) stamps `catalog_source = kLocal` on the stubs it
  produces (the only local-specific change). `FetchAllModelInfosWithCachedModels`
  ([catalog_client.cc](../src/catalog/catalog_client.cc)) — which now only fetches latest + resolves
  cached ids by-id — is untouched.
- [src/manager.cc](../src/manager.cc) / [src/manager.h](../src/manager.h) — build the `[Public]`
  source (~L325), construct the factory + `ModelCatalog` (factory `CreateModel` at ~L545)
- [src/catalog/catalog_cache.h](../src/catalog/catalog_cache.h) /
  [src/catalog/catalog_cache.cc](../src/catalog/catalog_cache.cc) — round-trips `catalog_source`
  via `ModelInfo` JSON
- [include/foundry_local/foundry_local_c.h](../include/foundry_local/foundry_local_c.h) /
  [src/c_api.cc](../src/c_api.cc) — read-only `catalog_source` (int) on `flModelInfo`;
  `Model_GetVariantsImpl` switches from `Variants()` to `UniqueVariants()` so the public list is
  de-duplicated. Any Private-catalog C-API surface stays **append-only** and is a future follow-up.
- [src/service/models_handlers.cc](../src/service/models_handlers.cc) — `OpenAIListModelsHandler`
  (`GET /v1/models`) switches from `Variants()` to `UniqueVariants()` so it never emits duplicate
  `model_id`s across catalog sources
- [CMakeLists.txt](../CMakeLists.txt) — add/remove sources

**Remove / absorb**
- `catalog/base_model_catalog.{h,cc}` → `catalog/model_catalog.{h,cc}`
- `catalog/azure_model_catalog.{h,cc}` → `catalog/azure_model_source.{h,cc}` (keeps the inline
  local-cache resolution: `ScanLocalModels` + `GetLiveCatalogOrLocalSnapshot` + `AddLocalModels` /
  `MakeByomModelInfo`, incl. the snapshot fallback and `CreateCatalogClient` seam)
- `FetchAllModelInfosWithCachedModels` ([catalog/catalog_client.cc](../src/catalog/catalog_client.cc))
  **stays** — reused by `AzureModelSource`; unchanged (stub synthesis lives in `MakeByomModelInfo`).
  `ICatalogClient` in `catalog_client.h` stays (used by `AzureModelSource`).

## Verification

1. `python sdk_v2/cpp/build.py --build --config Debug`
2. `foundry_local_tests.exe --gtest_filter="ModelCatalog*:*Source*:*Catalog*"` (fast; no model load)
3. Optional live: `sdk_integration_tests` `catalog_live` (real Azure fetch)
4. C# tests auto-load the fresh native via `foundry_local.native.cfg` — keep C ABI additions
   **append-only** so struct layout stays stable.

## Scope boundaries

- **Included (initial scope)**: the Public (Azure) source with inline local-cache resolution (cached
  `kPublic` entries + short-term `kLocal` orphan stubs, per *What "local" means*); the
  shadow / preference / `UniqueVariants` machinery and the `Unregister` / `RemoveVariant` path as
  dormant foundation.
- **Excluded (future follow-ups)**: the **Private** catalog and the dedicated **BYOM local** catalog
  (which replaces the interim `kLocal` stubs and adds `Register` / `Unregister`). Both are new
  sources, not redesigns; details TBD.
