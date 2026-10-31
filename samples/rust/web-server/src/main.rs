use foundry_local_sdk::{FoundryLocalConfig, FoundryLocalManager};
use serde_json::{json, Value};

const MODEL_ALIAS: &str = "qwen2.5-0.5b";

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("foundry_local_samples"))?;
    manager.download_and_register_eps(None).await?;
    let model = manager.catalog().get_model(MODEL_ALIAS).await?;

    if !model.is_cached().await? {
        model.download(None::<fn(f64)>).await?;
    }
    model.load().await?;

    manager.start_web_service().await?;
    let endpoint = manager
        .urls()?
        .into_iter()
        .next()
        .ok_or("The built-in service did not report a URL")?;
    println!("Built-in service: {endpoint}");

    let response = reqwest::Client::new()
        .post(format!(
            "{}/v1/chat/completions",
            endpoint.trim_end_matches('/')
        ))
        .json(&json!({
            "model": model.id(),
            "messages": [
                { "role": "user", "content": "Reply with a short hello from Foundry Local." }
            ],
            "max_tokens": 64,
            "stream": false
        }))
        .send()
        .await?
        .error_for_status()?
        .json::<Value>()
        .await?;

    let message = response
        .pointer("/choices/0/message/content")
        .and_then(Value::as_str)
        .unwrap_or("<no message returned>");
    println!("Assistant: {message}");

    manager.stop_web_service().await?;
    model.unload().await?;
    manager.shutdown()?;
    Ok(())
}
