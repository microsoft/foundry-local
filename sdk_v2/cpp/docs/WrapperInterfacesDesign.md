# C++ Wrapper Interfaces & Const-Handle Design

> Design document for the C++ wrapper refactor that introduces `IModel` / `ICatalog`
> interfaces, collapses the `Basic*<T>` / `Const*` / `*` triple-class pattern into a
> single class per concept, and adds dual mutable/const pointer support to
> `detail::Base<T>`.

## Status

**Complete.** Implemented and verified against the unit-test suite (`foundry_local_tests.exe`,
696/696 passing) and targeted integration tests (`sdk_integration_tests.exe` with
`--gtest_filter`). Pre-V1; no backward-compatibility constraints applied.

## Problem Statement

The current C++ wrapper has three structural issues:

1. **Triple-class pattern is verbose and inconsistent.** Each opaque C type produces a
   `BasicX<T>` template, a `using ConstX = BasicX<const flX>;` alias, and a derived
   `class X : public BasicX<flX>`. This pattern repeats for `Model`, `Item`, and
   `KeyValuePairs`. Each class re-exports protected members via `using`-declarations,
   which is boilerplate-heavy and obscures the actual API.

2. **No interface seams for testing.** Users who want to unit-test code that consumes
   the SDK have no way to substitute a mock `Model` or `Catalog`. The wrapper types
   are templates over a C struct, which can't be mocked.

3. **`detail::Base<T>` const-handling forces template proliferation.** `Base<const T>`
   and `Base<T>` are unrelated types, so const vs. mutable is enforced via separate
   class instantiations rather than at the method level.

## Design Decisions

### 1. `detail::Base<T>` — dual pointer storage

`Base<T>` becomes a single (non-template-on-const) class that stores both a const and
a mutable pointer to the same C object.

- Construction from `T*` sets both pointers; destructor calls release on the mutable
  pointer.
- Construction from `const T*` sets only the const pointer; destructor never releases.
- `get()` returns `const T*` and always works.
- `get_mutable()` returns `T*` and throws `foundry_local::Error` (with
  `FOUNDRY_LOCAL_ERROR_INVALID_USAGE`) if constructed from a const pointer. Using
  the SDK's own exception type — rather than `std::logic_error` — lets consumers
  catch all SDK-originated misuse uniformly and inspect `Code()`. The `Error` class
  is therefore declared above `namespace detail` in the header.

The C API enforces correct construction at compile time: a function that returns
`const flX**` cannot be passed to the mutable constructor without an explicit cast.
The runtime check in `get_mutable()` is a safety net for internal bugs, not a
user-facing failure mode.

### 2. Single class per concept (composition, not inheritance)

The `BasicX<T>` / `ConstX` / `X` triples collapse into one class each:

| Old | New |
|---|---|
| `BasicModel<T>`, `ConstModel`, `Model` | `Model` |
| `BasicItem<T>`, `ConstItem`, `Item` | `Item` |
| `BasicKeyValuePairs<T>`, `ConstKeyValuePairs`, `KeyValuePairs` | `KeyValuePairs` |

Each class **composes** a `detail::Base<T>` member rather than inheriting from it.
Composition is consistent across the wrapper, removes the `using`-declaration
boilerplate, and decouples the public interface from the RAII helper.

Each class provides two constructors mirroring `Base<T>`:
- `X(flX& raw)` — mutable / owning (with optional release function).
- `X(const flX& raw)` — read-only view, never owns.

Both public adoption ctors take **references**, not pointers. A null `flX*` becomes
a compile error rather than a deferred crash on first use; this matches `ModelList`'s
long-standing reference ctor and removes a class of silent footguns. Internal
`detail::Base<T>` ctors still take pointers — they need null/move-from semantics —
but user code never sees them.

Read-only methods are `const` and call `get()`. Mutating methods are non-`const` and
call `get_mutable()`.

### 3. `IModel` and `ICatalog` interfaces

Add abstract interfaces for `Model` and `Catalog` to enable user-side mocking.

Identity accessors (`Id`, `Name`, `Version`, `Alias`, `Uri`, etc.) are not duplicated
on `IModel`; callers go through `GetInfo()` and use `ModelInfo`'s accessors. This
keeps the interface narrow and avoids drift between the two surfaces.

```cpp
class IModel {
 public:
  virtual ~IModel() = default;
  virtual ModelInfo GetInfo() const = 0;
  virtual bool IsCached() const = 0;
  virtual bool IsLoaded() const = 0;
  virtual std::string_view GetPath() const = 0;
  virtual InputOutputInfo GetInputOutputInfo() const = 0;
  virtual ModelList GetVariants() const = 0;
  virtual void SelectVariant(const IModel& variant) = 0;
  virtual void Download(std::function<bool(float)> progress = nullptr) = 0;
  virtual void Load() = 0;
  virtual void Unload() = 0;
  virtual void RemoveFromCache() = 0;
};

class ICatalog {
 public:
  virtual ~ICatalog() = default;
  virtual std::string_view GetName() const = 0;
  virtual ModelList GetModels() const = 0;
  virtual ModelList GetCachedModels() const = 0;
  virtual ModelList GetLoadedModels() const = 0;
  virtual std::unique_ptr<IModel> GetModel(const std::string& alias) const = 0;
  virtual std::unique_ptr<IModel> GetModelVariant(const std::string& model_id) const = 0;
  virtual std::unique_ptr<IModel> GetLatestVersion(const IModel& model) const = 0;
};
```

`Model : public IModel` and `Catalog : public ICatalog`. Both are concrete, move-only,
hold a `detail::Base<flX>` member, and additionally expose `native_handle()` /
`native_handle_mutable()` accessors that return the underlying C pointer for use by
`Session` and other wrapper code that needs to call into the C API.

### 4. No interfaces for `Session`, `Request`, `Response`, or other types

Sessions are not wrapped in an interface. Rationale:

- The session family will grow (chat, audio, multi-modal, embeddings, predictive,
  realtime, etc.). Each has a different surface; one-size-fits-all interfaces would
  be either too generic to be useful or proliferate without bound.
- The standard ports-and-adapters pattern lets users define their own facade tailored
  to their use case, internally composing a concrete `foundry_local::ChatSession`.
  A user's `IMyChatSession` mock never has to construct or consume a `Response` —
  it returns whatever shape the user's interface specifies.
- `Request` and `Response` are SDK-internal types that only appear inside the user's
  wrapper, never at their facade boundary. They don't need interfaces or
  test-construction support.

`IModel` and `ICatalog` are exceptions because user facades naturally want to
**return** "a model" or "a catalog," and users cannot fabricate a `Model` from
anything other than an `flModel*` they don't have.

### 5. API shape changes

Driven by the interface decisions:

- `Manager::GetCatalog()` returns `ICatalog&` (manager owns the catalog). The
  concrete `Catalog` also exposes a public `explicit Catalog(flCatalog&)` adoption
  ctor for the rare case where a user holds a raw `flCatalog*` from the C API and
  wants a wrapper directly. There is no `friend` relationship between `Manager` and
  `Catalog` — the public ctor is consistent with every other top-level wrapper.
- `Catalog::GetModel(alias)` and `Catalog::GetModelVariant(id)` return
  `std::unique_ptr<IModel>`. Null = not found.
- `Catalog::GetLatestVersion(const IModel&)` returns `std::unique_ptr<IModel>`.
- `Catalog::GetModels()` / `GetCachedModels()` / `GetLoadedModels()` return
  an iterable `ModelList`.
- `Session`, `ChatSession`, `AudioSession` constructors take `IModel&`.
- `IModel::SelectVariant(const IModel&)` and `Catalog::GetLatestVersion(const IModel&)`
  internally use `static_cast<const Model&>(arg).native_handle()`. RTTI is not used;
  the downcast is sound because `Model` is the only concrete `IModel` produced by
  the SDK.

### 6. Removed types

- `BasicModel<T>`, `ConstModel`
- `BasicItem<T>`, `ConstItem`
- `BasicKeyValuePairs<T>`, `ConstKeyValuePairs`

No backward-compatibility aliases. Pre-V1; clean break.

## Const-Safety Guarantees

The dual-pointer design preserves compile-time const safety wherever the C API
distinguishes const from mutable access:

| C API signature | Construction path | `get_mutable()` available? |
|---|---|---|
| `Out_ const flX**` | `X(const flX&)` ctor | No — throws `Error(INVALID_USAGE)` |
| `Outptr_ flX**` | `X(flX&, release_fn)` ctor (internal) | Yes |

Example: `flItemApi::GetMetadata` returns `const flKeyValuePairs**` while
`GetMutableMetadata` returns `flKeyValuePairs**`. The wrapper code routes them to
the const and mutable constructors respectively; users calling `Set()` or `Remove()`
on a `KeyValuePairs` constructed from `GetMetadata` would fail at runtime in
`get_mutable()`. Users never construct these wrappers directly — only the SDK
implementation does — so mismatched construction is an SDK bug, not a user error.

## Out of Scope (Future Work)

- **`ILogger` and a logging callback C API.** Defer until the C API exposes a hook
  for log events. Adding `ILogger` without a corresponding C API hook would only
  capture logs the C++ wrapper itself emits, which is essentially nothing.
- **Interfaces for session types.** Users compose concrete sessions inside their
  own facade interfaces (see Decision 4).
- **`IRequest` / `IResponse`.** Same rationale (see Decision 4).

## Example Impact

```cpp
// Before:
Catalog catalog = manager.GetCatalog();
auto model_opt = catalog.GetModel("phi-3.5-mini");
if (!model_opt) { return 1; }
Model model = std::move(*model_opt);
if (!model.IsCached()) model.Download(...);
if (!model.IsLoaded()) model.Load();
ChatSession session(model);

// After:
ICatalog& catalog = manager.GetCatalog();
auto model = catalog.GetModel("phi-3.5-mini");      // unique_ptr<IModel>
if (!model) { return 1; }
if (!model->IsCached()) model->Download(...);
if (!model->IsLoaded()) model->Load();
ChatSession session(*model);                         // takes IModel&
```

`ModelList` iteration:

```cpp
ModelList models = catalog.GetModels();
for (const std::unique_ptr<IModel>& m : models) {
  std::cout << m->GetAlias() << "\n";
}
```

## Implementation Phases

All phases complete. Listed here for historical reference.

1. ✅ **Core plumbing.** Rewrote `detail::Base<T>` with dual pointers. Converted
   `KeyValuePairs`, `Item`, and `Model` to single-class composition with const +
   mutable constructors. Added `native_handle()` / `native_handle_mutable()`
   accessors on `Model` and `Item`.
2. ✅ **Interfaces.** Added `IModel` / `ICatalog`. `Model final : public IModel`,
   `Catalog final : public ICatalog`. Updated `Catalog` and `ModelList` return
   types. `Session` / `ChatSession` / `AudioSession` / `EmbeddingsSession` accept
   `IModel&`.
3. ✅ **Sweep for consistency.** All wrappers compose over `detail::Base<T>`.
4. ✅ **API hardening.** Public adoption ctors converted from `T*` to `T&` to make
   null handles a compile error. `Catalog` adoption ctor made public; `friend class
   Manager` removed.
5. ✅ **Verified.** Clean build, 696/696 unit tests, targeted integration tests
   (`ModelFixture.CatalogValidation`, `LoadUnloadCycle`, `CacheOnlyTest.*`) all
   passing. All three examples (`basic_chat`, `realtime_audio`, `streaming_audio`)
   compile and run.
