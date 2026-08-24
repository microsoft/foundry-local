# Multi-Catalog Architecture Tradeoffs

> Status: Design recommendation  
> Scope: Public, private, and local/BYOM model catalogs in the Foundry Local SDK

## Executive summary

The SDK should keep **one aggregated catalog as the primary public experience**, backed by independently
identified model sources internally. An aggregated view optimizes for users who want to discover and use
all models available to their application without first understanding catalog topology. Separate public
catalog instances make collisions more explicit, but push discovery, aggregation, fallback, and policy into
every SDK consumer, language binding, CLI, and service.

This recommendation is close to the direction in [PR #1002](https://github.com/microsoft/foundry-local/pull/1002),
but the current collision and identity rules should not be treated as the final multi-catalog contract.
Before enabling genuine private or BYOM sources, the implementation needs stable source-instance identity,
source-qualified runtime/cache keys, explicit collision and "latest" rules, and per-source operational state.

This is **not** a recommendation to expose both aggregate and separate catalog object hierarchies. Internally,
each configured source (Azure public, private, or local) should retain its own fetch, cache, authentication,
refresh, and health state. In the user-facing SDK, `Manager.GetCatalog()` should return one `Catalog` whose
results combine all enabled sources. The SDK would not also expose separate `GetPublicCatalog()`,
`GetPrivateCatalog()`, or `GetLocalCatalog()` objects. If advanced selection becomes necessary, the aggregate
catalog can add source-qualified lookup or filtering without introducing a second catalog hierarchy.

## Options

### Aggregated public catalog

`Manager.GetCatalog()` returns one view over all configured public, private, and local sources. Source
provenance is metadata on each model and source precedence is SDK policy.

### Separate public catalog instances

The Manager exposes independently addressable catalogs, as proposed by
[PR #943](https://github.com/microsoft/foundry-local/pull/943). Users enumerate catalogs and query each one
independently. Any unified view is built by the application, CLI, or service.

## Tradeoff summary

| Area | Aggregated public catalog | Separate catalog instances |
|---|---|---|
| Ease of use | One place to list, find, download, and load models | Users must discover catalogs and choose where to query |
| Discoverability | All available models appear together | Fragmented unless each consumer implements aggregation |
| Duplicate IDs | Requires provenance, qualified identity, and resolution rules | Naturally scoped during catalog lookup |
| Reproducibility | Unqualified lookup may vary by source availability and policy | `(catalog, model_id)` is naturally more stable |
| Public API/ABI | Smaller surface; advanced controls can be deferred | Requires naming, enumeration, lookup, lifetime, and default rules |
| Internal complexity | Central merge, collision, refresh, and partial-failure policy | Simpler per catalog, but shared cache/load services still need qualified identity |
| Ecosystem complexity | Implemented once in the SDK | Repeated in SDK consumers, CLI, REST service, and bindings |
| Failure isolation | Must be preserved and surfaced explicitly | Natural per-catalog isolation |
| Multiple private sources | Needs a stable source-instance ID | Named instances model this naturally |
| Reversibility | Separate views can be added later | Published catalog APIs cannot be removed; aggregation would have to coexist |

## Does the SDK user need to know about catalogs?

Not necessarily for the default discovery path. An aggregate view can serve users who do not care about
source topology, while provenance and qualified operations can serve users who do. By contrast, separate
catalogs require every user to understand catalog topology even when it is irrelevant to their task.

Users do need source information when:

- Two sources publish the same model ID with different contents.
- Reproducible execution must pin the exact source and artifact.
- Applications present every source-specific copy to their users.
- Sources represent different tenants or trust boundaries.
- A source fails, is stale, or is unavailable.

These needs justify explicit provenance and qualified operations, but do not by themselves require multiple
public `Catalog` objects.

No expected usage mix has been established. The **BYOM PR walkthrough meeting chat** raised public plus a
small number of private models as a possible common scenario, but this was a hypothesis rather than a product
decision or supporting data. Usage data would improve confidence in the choice of default experience, but it
is not required to choose the more reversible architecture. Evidence that source-specific workflows,
multiple private catalogs, or trust-boundary isolation dominate would be required before committing to
separate catalog objects in the ABI.

## Duplicate model IDs and selection

Duplicate IDs are the most important risk in an aggregate view. Ordering duplicates chooses a winner but
does not make the ID unique.

### Identical artifacts

If immutable digests prove two entries are the same artifact, the aggregate may safely coalesce them for
normal use while retaining all provenance and alternate download locations.

### Different or unverifiable artifacts

Entries with the same model ID but different or unknown artifact identities must remain distinguishable.
The SDK should:

- Preserve each entry with a stable `source_id`.
- Use a qualified identity such as `(source_id, model_id, artifact_id)`.
- Avoid silently selecting between different artifacts. Unqualified lookup should report ambiguity or
  require an explicitly documented resolution policy.
- Not rewrite canonical model names with `-private` or `-local` suffixes. Qualification is clearer and
  does not alter publisher identity or version relationships.

The qualified identity must also be used internally. The current download path is derived from publisher
and model ID, and the loaded-model map is keyed by model ID. Separate catalog objects alone therefore do
not prevent two different artifacts from sharing cache or loaded-model state.

### "Latest" and default selection

The current proposal orders variants by device, version, creation time, model ID, and only then catalog
source. Consequently, a newer public version can become the alias default over an older private version.
Separately, cached variants can beat higher-priority sources for container default selection, while exact-ID
lookup uses source priority. These rules are deterministic but not obvious.

The product must explicitly choose and document one of these models:

1. **Global latest:** version/device precede source.
2. **Source override:** choose the preferred source, then its latest version.
3. **No implicit cross-source resolution:** require qualification when sources conflict.

Whichever default is selected, reproducible applications should persist the resolved `source_id`, exact
model ID, and immutable artifact identity. "Latest" is useful for discovery, not reproducible execution.

## Implementation considerations

### Aggregated view

Aggregation centralizes policy but requires:

- Per-source refresh, cache, authentication, health, and staleness state.
- Explicit aggregate completeness, so a partial result is not presented as complete success.
- Stable model handles across refresh where promised.
- Collision-aware indices and qualified runtime/cache identity.
- Deterministic ordering and documented fallback.
- Preferably concurrent source fetches followed by atomic publication of an immutable aggregate snapshot.

The source/store split in [MultiCatalogSupportPlan.md](MultiCatalogSupportPlan.md) is a good internal seam:
sources retrieve model information while one store owns models and indices. It should evolve from source
**kinds** (`public`, `private`, `local`) to stable source **instances**, because multiple private catalogs
cannot be distinguished by kind alone.

### Separate instances

Separate catalogs simplify source-scoped lookup and operational isolation, but add Manager and wrapper
complexity:

- Unique and stable catalog names.
- Enumeration and lookup APIs in every language binding.
- Catalog-handle lifetime and wrapper identity.
- Per-catalog metadata snapshots and synchronization.
- A stable definition of the no-argument `GetCatalog()`.
- REST and CLI changes to expose or aggregate non-default catalogs.

They do not remove the need for qualified internal cache and load identity because catalogs still share
Manager-level download and model-load services.

## API and ABI implications

Because additions to the C ABI are difficult to remove, the SDK should expose only concepts that are
expected to remain useful regardless of how the implementation evolves.

The aggregate approach can retain the existing single-catalog API and defer catalog-topology APIs. It may
eventually require provenance and source-qualified selection, but those capabilities can be designed when
real collision and multiple-private-source scenarios are understood.

The separate approach permanently exposes:

- Catalog naming.
- Catalog enumeration and lookup.
- Default-catalog selection rules.
- Per-catalog configuration concepts that may be incomplete for future private-catalog authentication and
  policy.

If separate access is added and aggregation is introduced later, both models must remain supported. This
makes separate catalog instances the less reversible API choice.

## Role of the current proof of concept

[PR #1002](https://github.com/microsoft/foundry-local/pull/1002) demonstrates that sources can be separated
from a shared model store and presented through an aggregate catalog. It should be evaluated as a proof of
concept, not as the final API or implementation contract.

The architecture decision should focus on the durable behavioral questions: whether users normally see one
catalog or several, how duplicate IDs are resolved, whether multiple private sources must be supported, how
source provenance is represented, and when users need explicit source selection. Exact API types and
lower-level implementation details should be designed after those behaviors are agreed.

## Recommendation

Adopt the following architecture:

- `Manager.GetCatalog()` remains the single aggregate catalog for normal discovery and use.
- Sources are independent internal instances with stable IDs, isolated cache metadata, refresh, health,
  authentication, and policy.
- Every model retains source kind, stable source ID, and immutable artifact identity where available.
- Unqualified lookup follows an explicit, documented policy and does not silently resolve
  different-artifact collisions.
- Qualified lookup or source filtering is added only when a concrete scenario requires advanced selection.
- Local registration/unregistration uses a narrow administrative API rather than requiring a public local
  catalog object.
- Separate public catalog handles are deferred until filtering and qualified lookup are proven insufficient.

This recommendation follows from three considerations: it provides a simple default without preventing
explicit source selection, centralizes collision and fallback policy instead of requiring every consumer to
reimplement it, and makes the smaller, more reversible ABI commitment. The implementation cost of aggregation
is contained within the SDK, while the usability and API costs of separate catalogs would be permanent and
borne by every consumer.

Choose separate public catalogs instead if evidence shows that same-ID/different-artifact collisions,
multiple private catalogs, strict tenant/trust isolation, or source-specific workflows are common rather
than exceptional. In the absence of that evidence, the aggregate public experience is the lower-regret
choice because it preserves the option to add explicit source access later without committing every user to
catalog-aware workflows now.

## Inputs considered

- [PR #1002: Add plan for supporting multiple catalogs better](https://github.com/microsoft/foundry-local/pull/1002)
- [PR #943: separate named catalog instances](https://github.com/microsoft/foundry-local/pull/943)
- [PR #928 catalog discussion](https://github.com/microsoft/foundry-local/pull/928#discussion_r3771277953)
- BYOM PR walkthrough meeting chat
