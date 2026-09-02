use foundry_local_sdk::{
    CatalogType, FoundryLocalConfig, FoundryLocalError, FoundryLocalManager, LogLevel,
    NativeErrorCode,
};
use std::fs;
use std::path::{Path, PathBuf};
use std::process;
use std::time::{SystemTime, UNIX_EPOCH};

struct TempTestDir {
    path: PathBuf,
}

impl TempTestDir {
    fn new(prefix: &str) -> Self {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system clock must be after Unix epoch")
            .as_nanos();
        let path = std::env::temp_dir().join(format!("{prefix}-{}-{nonce}", process::id()));
        fs::create_dir(&path).expect("create temporary BYOM directory");
        Self { path }
    }

    fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for TempTestDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

#[test]
fn catalog_configuration_builders_are_publicly_usable() {
    let _config = FoundryLocalConfig::new("public-api-test")
        .catalog_region("eastus")
        .catalog_url_with_filter("https://example.test/catalog", "task=chat-completion")
        .catalog_url("https://fallback.example.test/catalog");
}

#[test]
fn native_errors_expose_stable_and_unknown_codes() {
    let cancelled = FoundryLocalError::Native {
        code: NativeErrorCode::OperationCancelled,
        message: "cancelled".to_string(),
    };
    assert_eq!(
        cancelled.native_code(),
        Some(NativeErrorCode::OperationCancelled)
    );
    assert_eq!(cancelled.native_message(), Some("cancelled"));

    let unknown = FoundryLocalError::Native {
        code: NativeErrorCode::Unknown(42),
        message: "future native error".to_string(),
    };
    assert_eq!(unknown.native_code(), Some(NativeErrorCode::Unknown(42)));
    assert_eq!(unknown.native_message(), Some("future native error"));
}

#[tokio::test]
async fn byom_registration_round_trips_and_preserves_assets() {
    assert_eq!(CatalogType::default(), CatalogType::Public);

    let temp = TempTestDir::new("foundry-local-rust-byom");
    let model_path = temp.path().join("model");
    let config_path = model_path.join("genai_config.json");
    let sentinel_path = model_path.join("caller-owned.txt");
    fs::create_dir(&model_path).expect("create model directory");
    fs::write(
        &config_path,
        r#"{"model":{"type":"phi3","context_length":4096}}"#,
    )
    .expect("write model configuration");
    fs::write(&sentinel_path, b"caller-owned asset").expect("write sentinel asset");

    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("system clock must be after Unix epoch")
        .as_nanos();
    let name = format!("rust-byom-{}-{nonce}", process::id());
    let model_id = format!("{name}:7");
    let manager = FoundryLocalManager::create(
        FoundryLocalConfig::new(format!("rust-byom-{nonce}"))
            .app_data_dir(temp.path().join("appdata").to_string_lossy())
            .model_cache_dir(temp.path().join("cache").join("models").to_string_lossy())
            .logs_dir(temp.path().join("logs").to_string_lossy())
            .log_level(LogLevel::Warn)
            .service_endpoint("http://127.0.0.1:1"),
    )
    .expect("create cache-only manager");
    let catalog = manager.local_catalog().expect("get local catalog");
    assert_eq!(catalog.catalog_type(), CatalogType::Local);

    let mut metadata = catalog.create_model_info().expect("create metadata");
    metadata
        .set_string_property("task", "chat-completion")
        .expect("set task")
        .set_string_property("display_name", "Rust BYOM")
        .expect("set display name")
        .set_int_property("context_length", 4096)
        .expect("set context length");

    let model = catalog
        .register_model(&model_path.to_string_lossy(), &model_id, metadata)
        .await
        .expect("register model");
    let info = model.info().expect("read model metadata");
    assert_eq!(info.id, model_id);
    assert_eq!(info.name, name);
    assert_eq!(info.version, 7);
    assert_eq!(info.task.as_deref(), Some("chat-completion"));
    assert_eq!(info.display_name.as_deref(), Some("Rust BYOM"));
    assert_eq!(info.context_length, Some(4096));
    assert_eq!(
        fs::canonicalize(model.path().await.expect("read model path"))
            .expect("canonicalize model path"),
        fs::canonicalize(&model_path).expect("canonicalize expected path")
    );

    catalog
        .unregister_model(&model_id)
        .await
        .expect("unregister model");
    assert!(config_path.is_file());
    assert_eq!(
        fs::read(&sentinel_path).expect("read sentinel"),
        b"caller-owned asset"
    );
    assert!(catalog.get_model_variant(&model_id).await.is_err());

    drop(model);
    drop(catalog);
    manager.shutdown().expect("shut down manager");
}
