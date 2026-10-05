//! Model-free manager recreation regression, isolated from model-dependent tests.

use foundry_local_sdk::{FoundryLocalConfig, FoundryLocalManager, LogLevel};
use std::path::Path;

fn test_config() -> FoundryLocalConfig {
    let output = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../cpp/build/TestResults")
        .join(format!("native-manager-lifetime-{}", std::process::id()));
    FoundryLocalConfig::new("FoundryLocalManagerLifetimeTest")
        .app_data_dir(output.to_string_lossy().into_owned())
        .model_cache_dir(output.join("models").to_string_lossy().into_owned())
        .logs_dir(output.join("logs").to_string_lossy().into_owned())
        .service_endpoint("http://127.0.0.1:1")
        .log_level(LogLevel::Warn)
}

#[test]
fn create_succeeds_after_all_previous_handles_are_released() {
    for _ in 0..3 {
        let manager = FoundryLocalManager::create(test_config())
            .expect("native manager creation must succeed after all previous handles are released");
        assert!(
            !manager.catalog().name().is_empty(),
            "fresh manager must expose a working catalog"
        );
        drop(manager);
    }
}
