use foundry_local_sdk::{
    ChatSession, FoundryLocalConfig, FoundryLocalManager, Item, Request, RequestOptions,
    SearchOptions, ToolChoice, ToolDefinition,
};
use serde_json::json;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("foundry_local_samples"))?;
    manager.download_and_register_eps(None).await?;
    let model = manager.catalog().get_model("qwen2.5-0.5b").await?;
    if model.supports_tool_calling() != Some(true) {
        return Err("The selected qwen2.5-0.5b variant does not support tool calling".into());
    }
    println!(
        "Using tool-capable model: {} ({})",
        model.alias(),
        model.id()
    );

    if !model.is_cached().await? {
        model.download(None::<fn(f64)>).await?;
    }
    model.load().await?;

    let session = ChatSession::new(&model).await?;
    session
        .add_tool_definition(
            ToolDefinition::new(
                "get_weather",
                json!({
                    "type": "object",
                    "properties": {
                        "city": { "type": "string" }
                    },
                    "required": ["city"],
                    "additionalProperties": false
                })
                .to_string(),
            )
            .with_description("Get the current weather for a city."),
        )
        .await?;

    let request = Request::from_items(vec![Item::user_message(vec![Item::text(
        "What is the weather in Paris?",
    )])])
    .with_options(RequestOptions {
        search: SearchOptions {
            temperature: Some(0.0),
            max_output_tokens: Some(256),
            ..Default::default()
        },
        tool_choice: Some(ToolChoice::Required),
        ..Default::default()
    });

    let response = session.process_request(request).await?;
    let call = response
        .items
        .iter()
        .find_map(Item::as_tool_call)
        .ok_or("The model did not produce a tool call")?;
    println!("Tool call: {}({})", call.name, call.arguments);
    // This small model is sensitive to field order, so match the other samples' mock response.
    let result = r#"{"temperature":22,"unit":"celsius","condition":"sunny"}"#;

    let follow_up = Request::from_items(vec![Item::tool_result(
        call.call_id.clone(),
        result.to_string(),
    )])
    .with_options(RequestOptions {
        search: SearchOptions {
            temperature: Some(0.0),
            max_output_tokens: Some(256),
            ..Default::default()
        },
        tool_choice: Some(ToolChoice::None),
        ..Default::default()
    });
    let final_response = session.process_request(follow_up).await?;
    for item in &final_response.items {
        if let Some(message) = item.as_message() {
            println!("Assistant: {}", message.text());
        }
    }

    drop(session);
    model.unload().await?;
    manager.shutdown()?;
    Ok(())
}
