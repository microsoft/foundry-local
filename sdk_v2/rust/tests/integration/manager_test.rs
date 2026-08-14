use super::common;
use foundry_local_sdk::FoundryLocalManager;

#[test]
fn should_initialize_successfully() {
    let config = common::test_config();
    let manager = FoundryLocalManager::create(config);
    assert!(
        manager.is_ok(),
        "Manager creation failed: {:?}",
        manager.err()
    );
}

#[test]
fn should_return_catalog_with_non_empty_name() {
    let manager = common::get_test_manager();
    let catalog = manager.catalog();
    let name = catalog.name();
    assert!(!name.is_empty(), "Catalog name should not be empty");
}
