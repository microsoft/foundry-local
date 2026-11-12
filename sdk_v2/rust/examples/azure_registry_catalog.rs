use foundry_local_sdk::{CatalogOptions, CatalogType, FoundryLocalConfig, FoundryLocalManager};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("RegistryDemo"))?;
    let result = manager.download_and_register_eps(None).await?;
    if !result.success {
        return Err(result.status.into());
    }
    let options = CatalogOptions::from([
        ("name".into(), "azureml-dev".into()),
        ("credential".into(), "azure_cli".into()),
    ]);
    let catalog = manager.get_catalog_with_options(CatalogType::AzureRegistry, &options)?;
    for model in catalog.get_models().await? {
        for variant in model.variants() {
            println!("- {} {}", model.alias(), variant.id());
        }
    }
    Ok(())
}
