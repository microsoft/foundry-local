use foundry_local_sdk::{FoundryLocalConfig, FoundryLocalError, NativeErrorCode};

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
