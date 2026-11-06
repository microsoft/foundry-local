use foundry_local_sdk::{
    ChatSession, FoundryLocalConfig, FoundryLocalManager, Item, Request, TextKind,
};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let model_directory = std::env::args()
        .nth(1)
        .ok_or("Usage: byom <model-directory>")?;
    let model_directory = std::env::current_dir()?.join(model_directory);

    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("foundry_local_samples"))?;
    manager.download_and_register_eps(None).await?;
    let catalog = manager.local_catalog()?;
    let mut metadata = catalog.create_model_info()?;
    metadata.set_string_property("task", "chat-completion")?;
    let model = catalog
        .register_model(
            &model_directory.to_string_lossy(),
            "sample-byom:1",
            metadata,
        )
        .await?;

    let inference_result = async {
        model.load().await?;
        let session = ChatSession::new(&model).await?;
        let request = Request::from_items(vec![Item::user_message(vec![Item::text(
            "Why is the sky blue? Answer in two sentences.",
        )])]);
        let response = session.process_request(request).await?;
        for item in &response.items {
            if let Some(message) = item.as_message() {
                print!("Assistant: ");
                for part in &message.content {
                    if let Item::Text {
                        text,
                        kind: TextKind::Default,
                    } = part
                    {
                        print!("{text}");
                    }
                }
                println!();
            }
        }
        Ok::<(), foundry_local_sdk::FoundryLocalError>(())
    }
    .await;

    if model.is_loaded().await? {
        model.unload().await?;
    }
    catalog.unregister_model("sample-byom:1").await?;
    inference_result?;
    manager.shutdown()?;
    Ok(())
}
