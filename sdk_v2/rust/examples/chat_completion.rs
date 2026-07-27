//! Basic chat completion example demonstrating synchronous and streaming
//! usage of the Foundry Local SDK.
#![allow(deprecated)] // intentionally demonstrates the deprecated OpenAI facade

use std::io::{self, Write};

use foundry_local_sdk::{
    ChatCompletionRequestMessage, ChatCompletionRequestSystemMessage,
    ChatCompletionRequestUserMessage, FoundryLocalConfig, FoundryLocalError, FoundryLocalManager,
};
use tokio_stream::StreamExt;

/// Convenience alias matching the SDK's internal Result type.
type Result<T> = std::result::Result<T, FoundryLocalError>;

#[tokio::main]
async fn main() -> Result<()> {
    // ── 1. Initialise the manager ────────────────────────────────────────
    let config = FoundryLocalConfig::new("foundry_local_samples");

    let manager = FoundryLocalManager::create(config)?;

    // ── 2. List available models ─────────────────────────────────────────
    let models = manager.catalog().get_models().await?;
    println!("Available models:");
    for model in &models {
        println!("  • {} (id: {})", model.alias(), model.id());
    }

    // ── 3. Pick a model and ensure it is loaded ──────────────────────────
    // Prefer a known chat model; fall back to the first available.
    let model_alias = ["phi-3.5-mini", "phi-4-mini"]
        .iter()
        .find(|alias| models.iter().any(|m| m.alias() == **alias))
        .map(|s| s.to_string())
        .or_else(|| models.first().map(|m| m.alias().to_string()))
        .expect("No models available in the catalog");

    let model = manager.catalog().get_model(&model_alias).await?;

    if !model.is_cached().await? {
        println!("Downloading model '{}'…", model.alias());
        model
            .download(Some(|progress: f64| {
                println!("  {progress:.1}%");
            }))
            .await?;
    }

    println!("Loading model '{}'…", model.alias());
    model.load().await?;

    // ── 4. Synchronous chat completion ───────────────────────────────────
    let client = model.create_chat_client().temperature(0.7).max_tokens(256);

    let messages: Vec<ChatCompletionRequestMessage> = vec![
        ChatCompletionRequestSystemMessage::from("You are a helpful assistant.").into(),
        ChatCompletionRequestUserMessage::from("What is Rust's ownership model?").into(),
    ];

    println!("\n--- Synchronous completion ---");
    let response = client.complete_chat(&messages, None).await?;
    if let Some(choice) = response.choices.first() {
        if let Some(ref content) = choice.message.content {
            println!("Assistant: {content}");
        }
    }

    // ── 5. Streaming chat completion ─────────────────────────────────────
    println!("\n--- Streaming completion ---");
    let stream_messages: Vec<ChatCompletionRequestMessage> = vec![
        ChatCompletionRequestSystemMessage::from("You are a helpful assistant.").into(),
        ChatCompletionRequestUserMessage::from("Explain the borrow checker in two sentences.")
            .into(),
    ];

    print!("Assistant: ");
    let mut stream = client
        .complete_streaming_chat(&stream_messages, None)
        .await?;
    while let Some(chunk) = stream.next().await {
        let chunk = chunk?;
        if let Some(choice) = chunk.choices.first() {
            if let Some(ref content) = choice.delta.content {
                print!("{content}");
                io::stdout().flush().ok();
            }
        }
    }
    println!();

    // ── 6. Unload the model──────────────────────────────────────────────
    println!("\nUnloading model…");
    model.unload().await?;
    println!("Done.");

    Ok(())
}
