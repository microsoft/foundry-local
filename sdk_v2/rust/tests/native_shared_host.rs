use foundry_local_sdk::{
    ChatSession, FoundryLocalConfig, FoundryLocalManager, Item, Request, RequestOptions,
    SearchOptions,
};
use std::path::PathBuf;

fn configuration(root: &std::path::Path) -> FoundryLocalConfig {
    FoundryLocalConfig::new("rust-native-host-test")
        .app_data_dir(root.join("data").to_string_lossy())
        .model_cache_dir(root.join("models").to_string_lossy())
        .logs_dir(root.join("logs").to_string_lossy())
        .use_shared_host(true)
}

#[test]
#[ignore = "subprocess worker for packaged_native_host_runs_a_normal_rust_session"]
fn observe_shared_listener() {
    let root =
        PathBuf::from(std::env::var_os("FOUNDRY_TEST_OBSERVER_ROOT").expect("observer root"));
    let expected = std::env::var("FOUNDRY_TEST_OBSERVER_URL").expect("expected listener");
    let manager = FoundryLocalManager::create(configuration(&root)).expect("create observer");
    let urls = manager.urls().expect("observe without starting");
    assert_eq!(
        urls,
        if expected.is_empty() {
            vec![]
        } else {
            vec![expected]
        }
    );
}

fn check_observer(root: &std::path::Path, expected: &str) {
    let output = std::process::Command::new(std::env::current_exe().expect("test binary"))
        .args([
            "--ignored",
            "--exact",
            "observe_shared_listener",
            "--nocapture",
        ])
        .env("FOUNDRY_TEST_OBSERVER_ROOT", root)
        .env("FOUNDRY_TEST_OBSERVER_URL", expected)
        .output()
        .expect("run independent observer");
    assert!(
        output.status.success(),
        "observer failed:\n{}\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

// Separate integration-test process: the SDK deliberately cannot change hosting mode after initialization.
#[tokio::test]
async fn packaged_native_host_runs_a_normal_rust_session() {
    let cpp = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../cpp")
        .canonicalize()
        .expect("resolve C++ SDK directory");
    let root = cpp
        .join("build/TestResults")
        .join(format!("rust-native-host-{}", std::process::id()));
    let manager = FoundryLocalManager::create(configuration(&root)).expect("create shared manager");
    check_observer(&root, "");
    let catalog = manager.local_catalog().expect("get local catalog");
    let mut metadata = catalog.create_model_info().expect("create metadata");
    metadata
        .set_string_property("task", "coding")
        .expect("set coding task");
    let model = catalog
        .register_model(
            &cpp.join("test/testdata/tiny-paged-attention")
                .to_string_lossy(),
            "rust-native-host:1",
            metadata,
        )
        .await
        .expect("register bundled model");
    model.load().await.expect("load through packaged host");
    manager.start_web_service().await.expect("start listener");
    let urls = manager.urls().expect("running listener");
    check_observer(&root, urls.first().expect("bound listener"));
    let session = ChatSession::new(&model).await.expect("create session");
    let request = Request::from_items(vec![Item::user_message(vec![Item::text("Hello")])])
        .with_options(RequestOptions {
            search: SearchOptions {
                max_output_tokens: Some(4),
                ..Default::default()
            },
            ..Default::default()
        });
    let response = session
        .process_request(request)
        .await
        .expect("infer through packaged host");
    assert!(response.usage.completion_tokens > 0);
    assert_eq!(session.turn_count(), 1);
    drop(session);
    model.unload().await.expect("unload model");
    catalog
        .unregister_model("rust-native-host:1")
        .await
        .expect("unregister bundled model");
    drop(model);
    drop(catalog);
    manager.shutdown().expect("release shared owner");
}
