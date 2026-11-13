use foundry_local_sdk::{
    CatalogOptions, CatalogType, ChatSession, FoundryLocalConfig, FoundryLocalManager, Item,
    Request, TextKind,
};

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
    let model = catalog.get_model("qwen2.5-0.5b").await?;
    println!("Using {}", model.id());
    model.download(None::<fn(f64)>).await?;
    model.load().await?;
    {
        let session = ChatSession::new(&model).await?;
        let request = Request::from_items(vec![Item::user_message(vec![Item::text(
            "What is 2 + 2? Answer briefly.",
        )])]);
        let response = session.process_request(request).await?;
        for item in &response.items {
            if let Item::Message(message) = item {
                for part in &message.content {
                    if let Item::Text {
                        text,
                        kind: TextKind::Default,
                    } = part
                    {
                        print!("{text}");
                    }
                }
            }
        }
        println!();
    }
    model.unload().await?;
    Ok(())
}
