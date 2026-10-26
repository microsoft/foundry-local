use foundry_local_sdk::{
    AudioSession, CatalogType, ChatSession, EmbeddingsSession, FoundryLocalConfig,
    FoundryLocalError, FoundryLocalManager, Item, LogLevel, NativeErrorCode, Request,
    RequestOptions, SearchOptions,
};
use std::fs;
use std::path::PathBuf;
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
fn catalog_type_defaults_to_public() {
    assert_eq!(CatalogType::default(), CatalogType::Public);
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
async fn byom_registration_round_trips_and_runs_coding() {
    let temp = TempTestDir::new("foundry-local-rust-byom");
    let model_path = temp.path.join("model");
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
    let create_manager = || {
        FoundryLocalManager::create(
            FoundryLocalConfig::new(format!("rust-byom-{nonce}"))
                .app_data_dir(temp.path.join("appdata").to_string_lossy())
                .model_cache_dir(temp.path.join("cache").join("models").to_string_lossy())
                .logs_dir(temp.path.join("logs").to_string_lossy())
                .log_level(LogLevel::Warn)
                .service_endpoint("http://127.0.0.1:1"),
        )
        .expect("create cache-only manager")
    };
    let manager = create_manager();
    let catalog = manager.local_catalog().expect("get local catalog");
    assert_eq!(catalog.catalog_type(), CatalogType::Local);

    let mut metadata = catalog.create_model_info().expect("create metadata");
    metadata
        .set_string_property("task", "chat-completion")
        .expect("set task")
        .set_string_property("display_name", "Rust BYOM")
        .expect("set display name")
        .set_string_property("custom_marker", "persist-me")
        .expect("set custom marker")
        .set_int_property("context_length", 4096)
        .expect("set context length")
        .set_int_property("custom_count", 42)
        .expect("set custom count");

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

    drop(model);
    drop(catalog);
    manager.shutdown().expect("shut down first manager");
    drop(manager);

    let recreated_manager = create_manager();
    let recreated_catalog = recreated_manager
        .local_catalog()
        .expect("get recreated local catalog");
    let recreated = recreated_catalog
        .get_model_variant(&model_id)
        .await
        .expect("lookup model after manager recreation");
    assert_eq!(
        recreated
            .get_string_property("custom_marker")
            .expect("read recreated custom string")
            .as_deref(),
        Some("persist-me")
    );
    assert_eq!(
        recreated
            .get_int_property("custom_count", -1)
            .expect("read recreated custom integer"),
        42
    );

    recreated_catalog
        .unregister_model(&model_id)
        .await
        .expect("unregister model");
    assert!(config_path.is_file());
    assert_eq!(
        fs::read(&sentinel_path).expect("read sentinel"),
        b"caller-owned asset"
    );
    assert!(recreated_catalog
        .get_model_variant(&model_id)
        .await
        .is_err());

    drop(recreated);
    let coding_path = fs::canonicalize(
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../cpp/test/testdata/tiny-paged-attention"),
    )
    .expect("resolve bundled coding test model");
    let mut coding_metadata = recreated_catalog
        .create_model_info()
        .expect("create coding metadata");
    coding_metadata
        .set_string_property("task", "coding")
        .expect("set coding task");
    let coding_model = recreated_catalog
        .register_model(
            &coding_path.to_string_lossy(),
            "rust-coding:1",
            coding_metadata,
        )
        .await
        .expect("register coding model");
    assert_eq!(
        coding_model
            .info()
            .expect("read coding metadata")
            .task
            .as_deref(),
        Some("coding")
    );
    coding_model.load().await.expect("load coding model");
    assert!(matches!(
        AudioSession::new(&coding_model).await,
        Err(FoundryLocalError::Validation { .. })
    ));
    assert!(matches!(
        EmbeddingsSession::new(&coding_model).await,
        Err(FoundryLocalError::Validation { .. })
    ));
    let session = ChatSession::new(&coding_model)
        .await
        .expect("create coding chat session");
    let request = Request::from_items(vec![Item::user_message(vec![Item::text(
        "Write a function.",
    )])])
    .with_options(RequestOptions {
        search: SearchOptions {
            max_output_tokens: Some(4),
            temperature: Some(0.0),
            ..Default::default()
        },
        ..Default::default()
    });
    let response = session
        .process_request(request)
        .await
        .expect("run coding inference");
    assert!(!response.items.is_empty());
    assert!(response.usage.completion_tokens > 0);
    assert_eq!(session.turn_count(), 1);
    drop(session);
    coding_model.unload().await.expect("unload coding model");
    recreated_catalog
        .unregister_model("rust-coding:1")
        .await
        .expect("unregister coding model");
    drop(coding_model);
    drop(recreated_catalog);
    recreated_manager.shutdown().expect("shut down manager");
}

#[test]
fn tool_definitions_preserve_the_published_struct_shape() {
    use foundry_local_sdk::{CustomToolDefinition, ToolDefinition};

    let literal = ToolDefinition {
        name: "literal".to_string(),
        description: None,
        json_schema: r#"{"type":"object"}"#.to_string(),
    };
    assert_eq!(literal.name, "literal");

    let function = ToolDefinition::new("multiply", r#"{"type":"object"}"#)
        .with_description("Multiplies two numbers.");
    assert_eq!(function.json_schema, r#"{"type":"object"}"#);
    assert_eq!(
        function.description.as_deref(),
        Some("Multiplies two numbers.")
    );

    let _custom = CustomToolDefinition::new("apply_patch").with_description("Applies a patch.");
}
