//! Interactive chat example — a simple terminal chatbot powered by Foundry Local.
//!
//! Run with: `cargo run --example interactive_chat`
#![allow(deprecated)] // intentionally demonstrates the deprecated OpenAI facade

use std::io::{self, Write};

use foundry_local_sdk::{
    ChatCompletionRequestAssistantMessage, ChatCompletionRequestMessage,
    ChatCompletionRequestSystemMessage, ChatCompletionRequestUserMessage, FoundryLocalConfig,
    FoundryLocalManager,
};
use tokio_stream::StreamExt;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    // ── Initialise ───────────────────────────────────────────────────────
    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("foundry_local_samples"))?;

    // Pick the first available model (or change this to a specific alias)
    let catalog = manager.catalog();
    let models = catalog.get_models().await?;

    println!("Available models:");
    for (i, m) in models.iter().enumerate() {
        println!("  [{i}] {}", m.alias());
    }

    print!("\nSelect a model number (default 0): ");
    io::stdout().flush()?;
    let mut choice = String::new();
    io::stdin().read_line(&mut choice)?;
    let idx: usize = choice.trim().parse().unwrap_or(0);

    let alias = models
        .get(idx)
        .map(|m| m.alias().to_string())
        .unwrap_or_else(|| models[0].alias().to_string());

    let model = catalog.get_model(&alias).await?;

    // Download if needed
    if !model.is_cached().await? {
        println!("Downloading '{alias}'…");
        model.download(Some(|p: f64| print!("\r  {p:.1}%"))).await?;
        println!();
    }

    println!("Loading '{alias}'…");
    model.load().await?;
    println!("Ready! Type your messages below. Press Ctrl-D (or type 'quit') to exit.\n");

    // ── Chat loop ────────────────────────────────────────────────────────
    let client = model.create_chat_client().temperature(0.7).max_tokens(512);

    let mut messages: Vec<ChatCompletionRequestMessage> = vec![
        ChatCompletionRequestSystemMessage::from("You are a helpful, concise assistant.").into(),
    ];

    loop {
        print!("You: ");
        io::stdout().flush()?;

        let mut input = String::new();
        if io::stdin().read_line(&mut input)? == 0 {
            break; // EOF (Ctrl-D)
        }

        let input = input.trim();
        if input.is_empty() {
            continue;
        }
        if input.eq_ignore_ascii_case("quit") || input.eq_ignore_ascii_case("exit") {
            break;
        }

        messages.push(ChatCompletionRequestUserMessage::from(input).into());

        // Stream the response token by token
        print!("Assistant: ");
        io::stdout().flush()?;

        let mut full_response = String::new();
        let mut stream = client.complete_streaming_chat(&messages, None).await?;
        while let Some(chunk) = stream.next().await {
            let chunk = chunk?;
            if let Some(choice) = chunk.choices.first() {
                if let Some(ref content) = choice.delta.content {
                    print!("{content}");
                    io::stdout().flush().ok();
                    full_response.push_str(content);
                }
            }
        }
        println!("\n");

        // Add assistant reply to history for multi-turn conversation
        messages.push(
            ChatCompletionRequestAssistantMessage {
                content: Some(full_response.into()),
                ..Default::default()
            }
            .into(),
        );
    }

    // ── Cleanup ──────────────────────────────────────────────────────────
    println!("\nUnloading model…");
    model.unload().await?;
    println!("Goodbye!");

    Ok(())
}
