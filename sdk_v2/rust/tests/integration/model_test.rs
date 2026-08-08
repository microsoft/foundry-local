use super::common;
use std::sync::Arc;

// ── Cached model verification ────────────────────────────────────────────────

#[tokio::test]
async fn should_verify_cached_models_from_test_data_shared() {
    let manager = common::get_test_manager();
    let catalog = manager.catalog();
    let cached = catalog
        .get_cached_models()
        .await
        .expect("get_cached_models failed");

    let has_qwen = cached.iter().any(|m| m.alias() == common::TEST_MODEL_ALIAS);
    assert!(
        has_qwen,
        "'{}' should be present in cached models",
        common::TEST_MODEL_ALIAS
    );

    let has_whisper = cached
        .iter()
        .any(|m| m.alias() == common::WHISPER_MODEL_ALIAS);
    assert!(
        has_whisper,
        "'{}' should be present in cached models",
        common::WHISPER_MODEL_ALIAS
    );
}

// ── Load / Unload ────────────────────────────────────────────────────────────

#[tokio::test]
async fn should_load_and_unload_model() {
    let manager = common::get_test_manager();
    let catalog = manager.catalog();
    let model = catalog
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed");

    model.load().await.expect("model.load() failed");
    assert!(
        model.is_loaded().await.expect("is_loaded check failed"),
        "Model should be loaded after load()"
    );

    model.unload().await.expect("model.unload() failed");
    assert!(
        !model.is_loaded().await.expect("is_loaded check failed"),
        "Model should not be loaded after unload()"
    );
}

// ── Introspection ────────────────────────────────────────────────────────────

#[tokio::test]
async fn should_expose_alias() {
    let manager = common::get_test_manager();
    let model = manager
        .catalog()
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed");

    assert_eq!(model.alias(), common::TEST_MODEL_ALIAS);
}

#[tokio::test]
async fn should_expose_non_empty_id() {
    let manager = common::get_test_manager();
    let model = manager
        .catalog()
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed");

    println!("Model id: {}", model.id());

    assert!(
        !model.id().is_empty(),
        "Model id() should be a non-empty string"
    );
}

#[tokio::test]
async fn should_have_at_least_one_variant() {
    let manager = common::get_test_manager();
    let model = manager
        .catalog()
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed");

    let variants = model.variants();
    println!("Model has {} variant(s)", variants.len());

    assert!(
        !variants.is_empty(),
        "Model should have at least one variant"
    );
}

#[tokio::test]
async fn should_have_selected_variant_matching_id() {
    let manager = common::get_test_manager();
    let model = manager
        .catalog()
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed");

    // The model's id() should return the selected variant's id
    // info() delegates to the selected variant, so id() and info().id must agree
    assert_eq!(
        model.id(),
        model.info().id,
        "model.id() should match model.info().id (the selected variant's metadata)"
    );
}

#[tokio::test]
async fn should_report_cached_model_as_cached() {
    let manager = common::get_test_manager();
    let model = manager
        .catalog()
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed");

    let cached = model.is_cached().await.expect("is_cached() should succeed");
    assert!(
        cached,
        "Test model '{}' should be cached (from test-data-shared)",
        common::TEST_MODEL_ALIAS
    );
}

#[tokio::test]
async fn should_return_non_empty_path_for_cached_model() {
    let manager = common::get_test_manager();
    let model = manager
        .catalog()
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed");

    let path = model.path().await.expect("path() should succeed");
    println!("Model path: {}", path.display());

    assert!(
        !path.as_os_str().is_empty(),
        "Cached model should have a non-empty path"
    );
}

#[tokio::test]
async fn should_select_variant_by_model() {
    let manager = common::get_test_manager();
    let model = manager
        .catalog()
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed");

    // Remember the original selection so we can restore it afterward.
    let original_id = model.id().to_string();

    let first_variant = model.variants()[0].clone();
    let first_variant_id = first_variant.id().to_string();
    model
        .select_variant(&first_variant)
        .expect("select_variant should succeed");
    assert_eq!(
        model.id(),
        first_variant_id,
        "After select_variant, id() should match the selected variant"
    );

    // Restore the original variant so other tests sharing this
    // model via the catalog are not affected.
    model
        .select_variant_by_id(&original_id)
        .expect("restoring original variant should succeed");
}

#[tokio::test]
async fn should_select_variant_by_id() {
    let manager = common::get_test_manager();
    let model = manager
        .catalog()
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed");

    let original_id = model.id().to_string();

    let first_variant_id = model.variants()[0].id().to_string();
    model
        .select_variant_by_id(&first_variant_id)
        .expect("select_variant_by_id should succeed");
    assert_eq!(
        model.id(),
        first_variant_id,
        "After select_variant_by_id, id() should match the selected variant"
    );

    model
        .select_variant_by_id(&original_id)
        .expect("restoring original variant should succeed");
}

#[tokio::test]
async fn should_fail_to_select_unknown_variant() {
    let manager = common::get_test_manager();
    let model = manager
        .catalog()
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed");

    let result = model.select_variant_by_id("nonexistent-variant-id");
    assert!(
        result.is_err(),
        "select_variant_by_id with unknown ID should fail"
    );

    let err_msg = result.unwrap_err().to_string();
    assert!(
        err_msg.contains("not found"),
        "Error should mention 'not found': {err_msg}"
    );
}

// ── Load manager (core interop) ──────────────────────────────────────────────

async fn get_test_model() -> Arc<foundry_local_sdk::Model> {
    let manager = common::get_test_manager();
    let catalog = manager.catalog();
    catalog
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed")
}

#[tokio::test]
async fn should_load_model_using_core_interop() {
    let model = get_test_model().await;
    model.load().await.expect("model.load() failed");
    model.unload().await.expect("model.unload() failed");
}

#[tokio::test]
async fn should_unload_model_using_core_interop() {
    let model = get_test_model().await;
    model.load().await.expect("model.load() failed");
    model.unload().await.expect("model.unload() failed");
}

#[tokio::test]
async fn should_list_loaded_models_using_core_interop() {
    let manager = common::get_test_manager();
    let catalog = manager.catalog();

    let loaded = catalog
        .get_loaded_models()
        .await
        .expect("catalog.get_loaded_models() failed");

    let _ = loaded;
}

#[tokio::test]
#[ignore = "requires running web service"]
async fn should_load_and_unload_model_using_external_service() {
    if common::is_running_in_ci() {
        eprintln!("Skipping external-service test in CI");
        return;
    }

    let manager = common::get_test_manager();
    let model = get_test_model().await;

    manager
        .start_web_service()
        .await
        .expect("start_web_service failed");

    model
        .load()
        .await
        .expect("load via external service failed");

    model
        .unload()
        .await
        .expect("unload via external service failed");
}

#[tokio::test]
#[ignore = "requires running web service"]
async fn should_list_loaded_models_using_external_service() {
    if common::is_running_in_ci() {
        eprintln!("Skipping external-service test in CI");
        return;
    }

    let manager = common::get_test_manager();

    manager
        .start_web_service()
        .await
        .expect("start_web_service failed");

    let catalog = manager.catalog();
    let loaded = catalog
        .get_loaded_models()
        .await
        .expect("get_loaded_models via external service failed");

    let _ = loaded;
}
