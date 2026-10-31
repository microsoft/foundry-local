use foundry_local_sdk::{
    ChatSession, FoundryLocalConfig, FoundryLocalManager, Item, Request, TextKind,
};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(1);
    let alias = args
        .next()
        .ok_or("Usage: vision <model-alias> <image-path>")?;
    let image_path = args
        .next()
        .ok_or("Usage: vision <model-alias> <image-path>")?;
    let image_path = std::env::current_dir()?.join(image_path);

    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("foundry_local_samples"))?;
    manager.download_and_register_eps(None).await?;
    let model = manager.catalog().get_model(&alias).await?;
    if !model.is_cached().await? {
        model.download(None::<fn(f64)>).await?;
    }
    model.load().await?;
    let session = ChatSession::new(&model).await?;
    let request = Request::from_items(vec![Item::user_message(vec![
        Item::text("Describe this image in one short sentence."),
        Item::image_uri(image_path.to_string_lossy().into_owned(), None::<String>),
    ])]);

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

    drop(session);
    model.unload().await?;
    manager.shutdown()?;
    Ok(())
}
