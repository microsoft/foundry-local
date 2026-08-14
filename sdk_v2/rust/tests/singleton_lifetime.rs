//! Regression test for the process-wide manager singleton lifetime.
//!
//! This lives in its own integration binary (a separate process) on purpose:
//! the shared `tests/integration` binary pins a process-lifetime handle via
//! `get_test_manager`, so the "outer wrapper dropped while a derived handle
//! keeps the native manager alive" scenario can only be exercised in a process
//! that owns every handle itself.
//!
//! The native core allows only one live `flManager` per process
//! (`Manager::Create` throws `INVALID_USAGE` if one already exists) and requires
//! it to stay valid until every derived `Model`/`Session` is destroyed. Dropping
//! the outer [`FoundryLocalManager`] while holding a derived handle keeps the
//! inner native manager alive, so a subsequent `create` must reuse it rather
//! than attempt a second, rejected creation.

use std::path::PathBuf;
use std::sync::Arc;

use foundry_local_sdk::{FoundryLocalConfig, FoundryLocalError, FoundryLocalManager, LogLevel};

/// Walk upward from `CARGO_MANIFEST_DIR` until a `.git` entry is found.
fn repo_root() -> PathBuf {
    let mut current = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    loop {
        if current.join(".git").exists() {
            return current;
        }
        if !current.pop() {
            panic!(
                "could not locate repo root from {}",
                env!("CARGO_MANIFEST_DIR")
            );
        }
    }
}

/// Shared model-cache directory (matches the main integration suite) so
/// `get_cached_models` finds the models provisioned for CI.
fn model_cache_dir() -> PathBuf {
    if let Ok(env_path) = std::env::var("FOUNDRY_TEST_DATA_DIR") {
        let p = PathBuf::from(&env_path);
        if p.is_dir() {
            return p;
        }
    }
    repo_root()
        .parent()
        .expect("repo root has no parent")
        .join("test-data-shared")
}

fn test_config() -> FoundryLocalConfig {
    let logs_dir = repo_root().join("sdk_v2").join("rust").join("logs");
    FoundryLocalConfig::new("FoundryLocalSingletonTest")
        .model_cache_dir(model_cache_dir().to_string_lossy().into_owned())
        .logs_dir(logs_dir.to_string_lossy().into_owned())
        .log_level(LogLevel::Warn)
}

/// `true` when the error just means the native library isn't present, so the
/// test should skip rather than fail (e.g. local dev without a built core).
fn is_native_unavailable(err: &FoundryLocalError) -> bool {
    matches!(err, FoundryLocalError::LibraryLoad { .. })
}

/// Dropping the outer manager while a derived `Model` handle is still alive must
/// not tear down the native manager, and the next `create` must succeed by
/// rebuilding a wrapper around the live native manager instead of attempting a
/// second `Manager_Create`.
#[tokio::test]
async fn create_reuses_native_manager_after_outer_drop() {
    // 1. First creation. Skip cleanly when the native core isn't available.
    let first = match FoundryLocalManager::create(test_config()) {
        Ok(manager) => manager,
        Err(ref e) if is_native_unavailable(e) => {
            eprintln!("skipping singleton lifetime test: native library unavailable ({e})");
            return;
        }
        Err(e) => panic!("unexpected error creating manager: {e}"),
    };

    // 2. Sharing: while a handle is live, every `create` returns the SAME Arc.
    let shared = FoundryLocalManager::create(test_config())
        .expect("create while a handle is alive must return the shared instance");
    assert!(
        Arc::ptr_eq(&first, &shared),
        "concurrent handles must share one instance"
    );

    // 3. Obtain a derived handle (a catalog `Model`) that keeps the *inner*
    //    native manager alive independently of the outer wrapper.
    let cached = first
        .catalog()
        .get_cached_models()
        .await
        .expect("listing cached models must succeed");
    let Some(keep_alive) = cached.into_iter().next() else {
        eprintln!("skipping regression assertion: no cached models available to hold");
        return;
    };

    // 4. Drop every outer handle. The inner native manager stays alive via
    //    `keep_alive`, so the native `flManager` is NOT torn down.
    drop(shared);
    drop(first);

    // 5. Regression: with only a derived handle alive, `create` must succeed by
    //    rebuilding a wrapper around the live native manager. Before the fix
    //    this failed — the outer `Weak` had expired, so `create` attempted a
    //    second `Manager_Create` that the core rejected with `INVALID_USAGE`.
    let rebuilt = FoundryLocalManager::create(test_config())
        .expect("create must reuse the still-live native manager, not re-create it");

    // 6. The rebuilt wrapper is a fresh, functional handle over the same core.
    assert!(
        !rebuilt.catalog().name().is_empty(),
        "rebuilt manager must expose a working catalog"
    );

    drop(rebuilt);
    drop(keep_alive);
}
