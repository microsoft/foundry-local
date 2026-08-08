//! Web-service lifecycle regression tests.
//!
//! Runs in its own integration binary (separate process) so it owns every
//! handle itself — required to exercise the "outer wrapper dropped while a
//! derived handle keeps the native alive" path against a live web service.
//!
//! Skips cleanly when the native core is unavailable, or when it was built
//! without web-service support (`FOUNDRY_LOCAL_BUILD_SERVICE=OFF`), in which
//! case `start_web_service` returns an error rather than binding a socket.

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

/// Shared model-cache directory (matches the main integration suite).
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
    FoundryLocalConfig::new("FoundryLocalWebServiceTest")
        .model_cache_dir(model_cache_dir().to_string_lossy().into_owned())
        .logs_dir(logs_dir.to_string_lossy().into_owned())
        .log_level(LogLevel::Warn)
}

/// `true` when the error just means the native library isn't present.
fn is_native_unavailable(err: &FoundryLocalError) -> bool {
    matches!(err, FoundryLocalError::LibraryLoad { .. })
}

fn base_url(urls: &[String]) -> String {
    urls.first()
        .expect("bound url present")
        .trim_end_matches('/')
        .to_string()
}

/// Start the in-process web service, exercise it over HTTP, then stop it and
/// confirm the outer state is cleared.
///
/// Also regression-covers the singleton fix: after the outer manager is dropped
/// while a derived `Model` keeps the native alive, recreating must surface the
/// still-running service through `urls()` (the rebuilt wrapper seeds its URL
/// cache from the live native manager).
#[tokio::test]
async fn web_service_starts_serves_and_survives_wrap_existing() {
    // 1. Create; skip cleanly when the native core isn't available.
    let manager = match FoundryLocalManager::create(test_config()) {
        Ok(manager) => manager,
        Err(ref e) if is_native_unavailable(e) => {
            eprintln!("skipping web-service test: native library unavailable ({e})");
            return;
        }
        Err(e) => panic!("unexpected error creating manager: {e}"),
    };

    // 2. Start the service. A core built without web-service support returns an
    //    error here (StartWebService throws INVALID_USAGE) — skip, don't fail.
    if let Err(e) = manager.start_web_service().await {
        eprintln!("skipping web-service test: service unavailable (built without support?) ({e})");
        return;
    }

    let urls = manager.urls().expect("urls after start");
    assert!(
        !urls.is_empty(),
        "start_web_service must expose at least one bound url"
    );
    let base = base_url(&urls);

    let http = reqwest::Client::new();

    // 3. The service must answer basic, model-free endpoints.
    let status = http
        .get(format!("{base}/status"))
        .send()
        .await
        .expect("GET /status");
    assert!(
        status.status().is_success(),
        "GET /status must return 2xx, got {}",
        status.status()
    );

    let models = http
        .get(format!("{base}/v1/models"))
        .send()
        .await
        .expect("GET /v1/models");
    assert!(
        models.status().is_success(),
        "GET /v1/models must return 2xx, got {}",
        models.status()
    );

    // 4. Regression for the singleton fix: keep the native alive via a derived
    //    handle, drop the outer wrapper, then recreate. The rebuilt wrapper must
    //    report the still-running service through `urls()`.
    let cached = manager
        .catalog()
        .get_cached_models()
        .await
        .expect("list cached models");
    if let Some(keep_alive) = cached.into_iter().next() {
        let keep_alive: Arc<_> = keep_alive;
        drop(manager);

        let rebuilt = FoundryLocalManager::create(test_config())
            .expect("create must reuse the still-live native manager");
        assert_eq!(
            rebuilt.urls().expect("urls after wrap_existing"),
            urls,
            "wrap_existing must surface the running web service's urls, not report it stopped"
        );

        rebuilt
            .stop_web_service()
            .await
            .expect("stop via rebuilt wrapper");
        assert!(
            rebuilt.urls().expect("urls after stop").is_empty(),
            "urls must clear after stop"
        );
        drop(keep_alive);
    } else {
        // No cached model to hold the native alive — still validate stop.
        eprintln!("no cached models available; skipping wrap_existing assertion");
        manager.stop_web_service().await.expect("stop");
        assert!(
            manager.urls().expect("urls after stop").is_empty(),
            "urls must clear after stop"
        );
    }

    // 5. The socket should no longer accept connections (best-effort; the
    //    deterministic signal is the cleared URL cache asserted above).
    match http.get(format!("{base}/status")).send().await {
        Ok(resp) => eprintln!(
            "note: /status still answered after stop with {}",
            resp.status()
        ),
        Err(_) => { /* expected: connection refused */ }
    }
}
