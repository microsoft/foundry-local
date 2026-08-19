use super::common;
use foundry_local_sdk::FoundryLocalManager;
use std::sync::Arc;

fn manager() -> Arc<FoundryLocalManager> {
    common::get_test_manager()
}

#[test]
fn should_initialize_with_catalog_name() {
    let manager = manager();
    let cat = manager.catalog();
    let name = cat.name();
    assert!(!name.is_empty(), "Catalog name must not be empty");
}

#[tokio::test]
async fn should_list_models() {
    let manager = manager();
    let cat = manager.catalog();
    let models = cat.get_models().await.expect("get_models failed");

    assert!(
        !models.is_empty(),
        "Expected at least one model in the catalog"
    );

    let found = models.iter().any(|m| m.alias() == common::TEST_MODEL_ALIAS);
    assert!(
        found,
        "Test model '{}' not found in catalog",
        common::TEST_MODEL_ALIAS
    );
}

#[tokio::test]
async fn should_get_model_by_alias() {
    let manager = manager();
    let cat = manager.catalog();
    let model = cat
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed");

    assert_eq!(model.alias(), common::TEST_MODEL_ALIAS);
}

#[tokio::test]
async fn should_throw_when_getting_model_with_empty_alias() {
    let manager = manager();
    let cat = manager.catalog();
    let result = cat.get_model("").await;
    assert!(result.is_err(), "Expected error for empty alias");

    let err_msg = result.unwrap_err().to_string();
    assert!(
        err_msg.contains("Model alias must be a non-empty string"),
        "Unexpected error message: {err_msg}"
    );
}

#[tokio::test]
async fn should_throw_when_getting_model_with_unknown_alias() {
    let manager = manager();
    let cat = manager.catalog();
    let result = cat.get_model("unknown-nonexistent-model-alias").await;
    assert!(result.is_err(), "Expected error for unknown alias");

    let err_msg = result.unwrap_err().to_string();
    assert!(
        err_msg.contains("Unknown model alias"),
        "Error should mention unknown alias: {err_msg}"
    );
    assert!(
        err_msg.contains("Available"),
        "Error should list available models: {err_msg}"
    );
}

#[tokio::test]
async fn should_get_cached_models() {
    let manager = manager();
    let cat = manager.catalog();
    let cached = cat
        .get_cached_models()
        .await
        .expect("get_cached_models failed");

    assert!(!cached.is_empty(), "Expected at least one cached model");

    let found = cached.iter().any(|m| m.alias() == common::TEST_MODEL_ALIAS);
    assert!(
        found,
        "Test model '{}' should be in the cached models list",
        common::TEST_MODEL_ALIAS
    );
}

#[tokio::test]
async fn should_throw_when_getting_model_variant_with_empty_id() {
    let manager = manager();
    let cat = manager.catalog();
    let result = cat.get_model_variant("").await;
    assert!(result.is_err(), "Expected error for empty variant ID");
}

#[tokio::test]
async fn should_throw_when_getting_model_variant_with_unknown_id() {
    let manager = manager();
    let cat = manager.catalog();
    let result = cat
        .get_model_variant("unknown-nonexistent-variant-id")
        .await;
    assert!(result.is_err(), "Expected error for unknown variant ID");
}
