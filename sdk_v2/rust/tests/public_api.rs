use foundry_local_sdk::{
    Catalog, CatalogType, FoundryLocalConfig, FoundryLocalError, FoundryLocalManager, Model,
    ModelInfoBuilder, NativeErrorCode,
};
use std::future::Future;
use std::sync::Arc;

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

#[test]
fn byom_api_is_publicly_usable() {
    assert_eq!(CatalogType::default(), CatalogType::Public);
    assert_ne!(CatalogType::Public, CatalogType::Local);

    fn assert_register_future<F>(_: F)
    where
        F: Future<Output = Result<Arc<Model>, FoundryLocalError>>,
    {
    }

    fn assert_unregister_future<F>(_: F)
    where
        F: Future<Output = Result<(), FoundryLocalError>>,
    {
    }

    let _new_metadata: fn(&Catalog) -> Result<ModelInfoBuilder, FoundryLocalError> =
        Catalog::create_model_info;
    let _get_catalog: fn(&FoundryLocalManager, CatalogType) -> Result<Catalog, FoundryLocalError> =
        FoundryLocalManager::get_catalog;
    let _local_catalog: fn(&FoundryLocalManager) -> Result<Catalog, FoundryLocalError> =
        FoundryLocalManager::local_catalog;

    fn check_catalog_methods(catalog: &Catalog, metadata: ModelInfoBuilder) {
        assert_register_future(catalog.register_model("model", "local-model:1", metadata));
        assert_unregister_future(catalog.unregister_model("local-model:1"));
    }

    let _ = check_catalog_methods;
}
