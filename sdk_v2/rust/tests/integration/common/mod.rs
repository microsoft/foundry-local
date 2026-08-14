//! Shared test utilities and configuration for Foundry Local SDK integration tests.
//!
//! Mirrors `testUtils.ts` from the JavaScript SDK test suite.

#![allow(dead_code)]

use std::path::PathBuf;
use std::sync::{Arc, Mutex, Once, OnceLock};

use foundry_local_sdk::{FoundryLocalConfig, FoundryLocalManager, LogLevel};

/// Default model alias used for chat-completion integration tests.
pub const TEST_MODEL_ALIAS: &str = "qwen2.5-0.5b";

/// Default model alias used for audio-transcription integration tests.
pub const WHISPER_MODEL_ALIAS: &str = "whisper-tiny";

/// Default model alias used for embedding integration tests.
pub const EMBEDDING_MODEL_ALIAS: &str = "qwen3-embedding-0.6b";

/// Stable phrases expected in the shared audio test file's transcription.
pub const EXPECTED_TRANSCRIPTION_PHRASES: &[&str] = &[
    "more than one link",
    "behind the scenes photo gallery",
    "like these next few links",
];

/// Assert transcript content while ignoring punctuation, case, and whitespace variation.
pub fn assert_transcript_semantically_matches(text: &str) {
    fn normalize(value: &str) -> String {
        value
            .to_lowercase()
            .chars()
            .map(|ch| if ch.is_alphanumeric() { ch } else { ' ' })
            .collect::<String>()
            .split_whitespace()
            .collect::<Vec<_>>()
            .join(" ")
    }

    let normalized = normalize(text);
    for phrase in EXPECTED_TRANSCRIPTION_PHRASES {
        assert!(
            normalized.contains(&normalize(phrase)),
            "Transcription should contain '{phrase}', got: {text}"
        );
    }
}

// ── Environment helpers ──────────────────────────────────────────────────────

/// Returns `true` when the tests are running inside a CI environment
/// (Azure DevOps or GitHub Actions).
pub fn is_running_in_ci() -> bool {
    let azure_devops = std::env::var("TF_BUILD").unwrap_or_else(|_| "false".into());
    let github_actions = std::env::var("GITHUB_ACTIONS").unwrap_or_else(|_| "false".into());
    azure_devops.eq_ignore_ascii_case("true") || github_actions.eq_ignore_ascii_case("true")
}

/// Walk upward from `CARGO_MANIFEST_DIR` until a `.git` directory is found.
pub fn get_git_repo_root() -> PathBuf {
    let mut current = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    loop {
        if current.join(".git").exists() {
            return current;
        }
        if !current.pop() {
            panic!(
                "Could not locate git repo root starting from {}",
                env!("CARGO_MANIFEST_DIR")
            );
        }
    }
}

/// Path to the shared test-data directory.
/// Uses FOUNDRY_TEST_DATA_DIR env var if set (CI), otherwise falls back
/// to looking for test-data-shared as a sibling of the repo root.
pub fn get_test_data_shared_path() -> PathBuf {
    if let Ok(env_path) = std::env::var("FOUNDRY_TEST_DATA_DIR") {
        let p = PathBuf::from(&env_path);
        if p.is_dir() {
            return p;
        }
    }
    let repo_root = get_git_repo_root();
    repo_root
        .parent()
        .expect("repo root has no parent")
        .join("test-data-shared")
}

/// Path to the shared audio test file used by audio-client tests.
pub fn get_audio_file_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("testdata")
        .join("Recording.mp3")
}

// ── Test configuration ───────────────────────────────────────────────────────

/// Build a [`FoundryLocalConfig`] suitable for integration tests.
///
/// * `modelCacheDir`  → `<repo-root>/../test-data-shared`
/// * `logsDir`        → `<repo-root>/sdk_v2/rust/logs`
/// * `logLevel`       → `Warn`
pub fn test_config() -> FoundryLocalConfig {
    let repo_root = get_git_repo_root();
    let logs_dir = repo_root.join("sdk_v2").join("rust").join("logs");

    FoundryLocalConfig::new("FoundryLocalTest")
        .model_cache_dir(get_test_data_shared_path().to_string_lossy().into_owned())
        .logs_dir(logs_dir.to_string_lossy().into_owned())
        .log_level(LogLevel::Warn)
}

static TEST_MANAGER: OnceLock<Mutex<Option<Arc<FoundryLocalManager>>>> = OnceLock::new();
static REGISTER_MANAGER_CLEANUP: Once = Once::new();

extern "C" {
    fn atexit(callback: extern "C" fn()) -> std::os::raw::c_int;
}

extern "C" fn cleanup_test_manager() {
    let Some(slot) = TEST_MANAGER.get() else {
        return;
    };
    let manager = slot
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
        .take();
    if let Some(manager) = manager {
        let _ = manager.shutdown();
        drop(manager);
    }
}

/// Create (or return) the shared [`FoundryLocalManager`] for tests.
///
/// The manager remains shared for the test run, then an `atexit` callback drops
/// the final strong handle before the native library's C++ static destructors
/// run. This preserves one-time initialization without leaking the manager into
/// process teardown.
pub fn get_test_manager() -> Arc<FoundryLocalManager> {
    let slot = TEST_MANAGER.get_or_init(|| Mutex::new(None));
    let mut manager = slot.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    let manager = manager.get_or_insert_with(|| {
        FoundryLocalManager::create(test_config()).expect("Failed to create FoundryLocalManager")
    });
    let result = Arc::clone(manager);

    REGISTER_MANAGER_CLEANUP.call_once(|| {
        // SAFETY: the callback has C ABI, takes no arguments, and remains valid
        // for the process lifetime.
        let status = unsafe { atexit(cleanup_test_manager) };
        assert_eq!(status, 0, "failed to register test-manager cleanup");
    });

    result
}

/// Serialises the web-service integration tests.
///
/// The local web service is a single process-wide native resource and
/// [`FoundryLocalManager::start_web_service`] is not idempotent, yet Rust runs
/// `#[tokio::test]`s concurrently. Each web-service test holds this async lock
/// for its whole body so their start/stop lifecycles never overlap.
pub async fn web_service_guard() -> tokio::sync::MutexGuard<'static, ()> {
    static WEB_SERVICE_LOCK: OnceLock<tokio::sync::Mutex<()>> = OnceLock::new();
    WEB_SERVICE_LOCK
        .get_or_init(|| tokio::sync::Mutex::new(()))
        .lock()
        .await
}

// ── Tool definitions ─────────────────────────────────────────────────────────

/// Returns a tool definition for a simple "multiply" function.
///
/// Used by tool-calling chat-completion tests.
pub fn get_multiply_tool() -> foundry_local_sdk::ChatCompletionTools {
    serde_json::from_value(serde_json::json!({
        "type": "function",
        "function": {
            "name": "multiply",
            "description": "Multiply two numbers together",
            "parameters": {
                "type": "object",
                "properties": {
                    "a": {
                        "type": "number",
                        "description": "The first number"
                    },
                    "b": {
                        "type": "number",
                        "description": "The second number"
                    }
                },
                "required": ["a", "b"]
            }
        }
    }))
    .expect("Failed to parse multiply tool definition")
}
