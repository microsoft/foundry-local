# Multi-Catalog Architecture Tradeoffs

> Status: Design analysis and conditional recommendation
> Scope: Public, private, and local/BYOM model catalogs in the Foundry Local SDK

## Executive summary

The SDK integrator must configure and manage model sources, so aggregation does not make catalog topology
unknown to that developer. The decision is whether the SDK should provide one combined query surface or
expose separate catalog instances for the application to use or present to its users.

The right choice depends primarily on the application experience. If application users see one model list,
SDK aggregation provides consistent merging, collision handling, and fallback. If users choose a catalog or
catalog identity represents a tenant, trust boundary, or product grouping, separate instances map more
directly to that experience and SDK aggregation offers less benefit.

In either design, configured sources should remain operationally independent internally, with separate
identity, fetch, cache, authentication, refresh, and health state. The SDK should avoid exposing both complete
aggregate and separate catalog object hierarchies unless a concrete scenario requires both.

## Options

### Single aggregated SDK catalog

`Manager.GetCatalog()` returns one view over all configured public, private, and local sources. Source
provenance is metadata on each model and source precedence is SDK policy.

### Separate SDK catalog instances

The Manager exposes independently addressable catalogs, as proposed by
[PR #943](https://github.com/microsoft/foundry-local/pull/943). Users enumerate catalogs and query each one
independently. Any unified view is built by the application, CLI, or service.

## Tradeoff summary

| Area | Single aggregated SDK catalog | Separate SDK catalog instances |
|---|---|---|
| SDK integrator experience | One query surface, but the integrator still configures and understands its sources | Explicit source selection and application-owned composition |
| Application-user experience | Natural for one combined model list | Natural for catalog selection, grouping, or trust boundaries |
| Discoverability | SDK provides a combined result | Application merges results or presents per-catalog navigation |
| Duplicate IDs | Requires provenance, qualified identity, and resolution rules | Naturally scoped during catalog lookup |
| Reproducibility | Unqualified lookup may vary by source availability and policy | `(catalog, model_id)` is naturally more stable |
| Public API/ABI | Smaller surface; advanced controls can be deferred | Requires naming, enumeration, lookup, lifetime, and default rules |
| Internal complexity | Central merge, collision, refresh, and partial-failure policy | Simpler per catalog, but shared cache/load services still need qualified identity |
| Policy ownership | SDK defines merging, collision, and fallback consistently | Each application intentionally defines or avoids cross-catalog policy |
| Failure isolation | Must be preserved and surfaced explicitly | Natural per-catalog isolation |
| Multiple private sources | Needs a stable source-instance ID | Named instances model this naturally |
| Reversibility | Separate views can be added later | Published catalog APIs cannot be removed; aggregation would have to coexist |

## Who needs to know about catalogs?

The SDK integrator is aware of catalog topology in either design because they configure the sources. The
material question is whether catalog identity is also part of the application-user experience:

- **Catalogs are internal to the application.** Application users see models, not catalogs. Aggregation is a
  convenience and consistency benefit, but the SDK integrator could instead merge or route between separate
  catalogs.
- **The application exposes one combined model list.** Aggregation has stronger value because the SDK can
  provide shared ordering, collision, fallback, and pagination behavior rather than each application
  implementing it.
- **The application exposes catalog selection or grouping.** Separate instances are a more natural model.
  An aggregate view may still be useful as an optional "all catalogs" experience, but should not replace
  explicit catalog identity.

Regardless of presentation, source information is required when two sources publish different artifacts
under the same ID, reproducible execution must pin an artifact, sources represent trust boundaries, or the
application must explain source health and availability.

These needs justify explicit provenance and qualified operations, but do not by themselves require multiple
separate `Catalog` objects.

No expected usage mix or application presentation model has been established. The **BYOM PR walkthrough
meeting chat** raised public plus a small number of private models as a possible scenario, but this was a
hypothesis rather than a product decision or supporting data. Before choosing the final public API, product
input is required on whether application users see one model universe, choose among catalogs, or never see
catalog identity at all. Usage frequency would help prioritize the default but is secondary to that product
contract.

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

First decide whether catalog identity is a first-class concept in the application-user experience:

- If application users consume one combined model universe, use a single aggregated SDK catalog. This
  centralizes cross-source policy and gives the CLI, service, and language bindings consistent behavior.
- If application users choose catalogs, or catalogs represent product groupings, tenants, or trust
  boundaries, expose separate SDK catalog instances and let the application decide whether to add an
  "all catalogs" view.
- If catalogs are entirely internal and applications do not need consistent cross-source behavior, either
  option is viable; aggregation is primarily convenience, while separate instances give the integrator more
  policy control.

In all cases, keep source instances independent internally and preserve source/artifact identity. Do not
silently resolve same-ID entries that may represent different artifacts.

Until the application presentation model is decided, [PR #1002](https://github.com/microsoft/foundry-local/pull/1002)
should remain a proof of concept rather than establishing the final public API. The ABI reversibility of a
single catalog is a reason to avoid premature catalog-topology APIs, but it is not sufficient by itself to
override a product requirement for explicit catalog selection.

## Inputs considered

- [PR #1002: Add plan for supporting multiple catalogs better](https://github.com/microsoft/foundry-local/pull/1002)
- [PR #943: separate named catalog instances](https://github.com/microsoft/foundry-local/pull/943)
- [PR #928 catalog discussion](https://github.com/microsoft/foundry-local/pull/928#discussion_r3771277953)
- BYOM PR walkthrough meeting chat
