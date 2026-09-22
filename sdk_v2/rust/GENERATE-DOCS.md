# Generating API Reference Docs

There are two API references for the Rust SDK:

1. **`cargo doc` (HTML) — canonical.** Generated directly from the `///` doc
   comments in `src/`. Always in sync with the source.
2. **`docs/api.md` (markdown) — hand-maintained stopgap.** A committed snapshot
   of the public API, kept until the crate is published to crates.io / docs.rs.
   It is **not** auto-generated — update it by hand when the public API changes.

## Generate & view the HTML locally

```bash
cd sdk_v2/rust
cargo doc --no-deps --open
```

This builds HTML at `target/doc/foundry_local_sdk/index.html` and opens it. To
include internal/private items while working on the SDK:

```bash
cargo doc --no-deps --document-private-items --open
```

## Check for broken docs (CI)

Doc comments can rot — a renamed type breaks an intra-doc `[link]`, or an example
stops compiling. Treat doc warnings as errors to catch this:

```bash
# Broken intra-doc links, missing-doc warnings, etc.
RUSTDOCFLAGS="-D warnings" cargo doc --no-deps

# Compile-check the ```rust examples in doc comments
cargo test --doc
```

On Windows PowerShell:

```powershell
$env:RUSTDOCFLAGS = "-D warnings"; cargo doc --no-deps
```

## Maintaining `docs/api.md`

`docs/api.md` is updated manually. When you add, remove, or change the signature
of a public item, update the corresponding row/section in `api.md` in the same
change. rustdoc has no first-party markdown output, so there is no command that
regenerates this file — treat the `///` doc comments as the source of truth and
mirror the public surface into `api.md`.

Once the crate is published, [docs.rs](https://docs.rs/foundry-local-sdk) will
host the `cargo doc` output for every release (the build script skips native
acquisition under `DOCS_RS`; see `build.rs`), and `api.md` can be retired in
favour of that link.

## Public API surface

The SDK re-exports its public types from the crate root. Key entry points:

| Type | Description |
|---|---|
| `FoundryLocalManager` | Entry point — SDK initialisation, web service lifecycle |
| `FoundryLocalConfig` | Configuration (app name, log level, catalog, service endpoint) |
| `Catalog` | Model discovery, lookup, and version queries |
| `Model` | Grouped model (alias → best variant) — download, load, unload |
| `Session` / `ChatSession` / `EmbeddingsSession` / `AudioSession` | Inference sessions |
| `ChatClient` / `AudioClient` | OpenAI-compatible clients (sync + streaming) |
| `FoundryLocalError` / `NativeErrorCode` | Error enum and stable native error codes |


