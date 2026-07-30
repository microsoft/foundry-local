//! Low-level inference example using the modality-agnostic `Session` / `Item` /
//! `Request` / `Response` API (as opposed to the higher-level OpenAI facade).
//!
//! Demonstrates a synchronous request, a streaming request, and multi-turn chat
//! with `ChatSession`.

use std::io::{self, Write};

use foundry_local_sdk::{
    ChatSession, FoundryLocalConfig, FoundryLocalError, FoundryLocalManager, Item, MessageRole,
    Request,
};
use tokio_stream::StreamExt;

/// Convenience alias matching the SDK's internal Result type.
type Result<T> = std::result::Result<T, FoundryLocalError>;

#[tokio::main]
async fn main() -> Result<()> {
    // ── 1. Initialise the manager and load a chat model ──────────────────
    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("foundry_local_samples"))?;

    let models = manager.catalog().get_models().await?;
    let model_alias = ["phi-3.5-mini", "phi-4-mini"]
        .iter()
        .find(|alias| models.iter().any(|m| m.alias() == **alias))
        .map(|s| s.to_string())
        .or_else(|| models.first().map(|m| m.alias().to_string()))
        .expect("No models available in the catalog");

    let model = manager.catalog().get_model(&model_alias).await?;
    if !model.is_cached().await? {
        println!("Downloading model '{}'…", model.alias());
        model.download(None::<fn(f64)>).await?;
    }
    println!("Loading model '{}'…", model.alias());
    model.load().await?;

    // ── 2. Open a chat session ───────────────────────────────────────────
    // `ChatSession` derefs to `Session`, so `process_request` /
    // `process_streaming_request` are available directly, plus turn management.
    let session = ChatSession::new(&model).await?;

    // ── 3. Synchronous request ───────────────────────────────────────────
    // A `Request` is pure data: a list of `Item`s. Here a single user message.
    let request = Request::from_items(vec![Item::message(
        MessageRole::User,
        vec![Item::text("What is Rust's ownership model?")],
    )]);

    println!("\n--- Synchronous request ---");
    let response = session.process_request(request).await?;
    println!("Assistant: {}", response.text());
    println!(
        "(finish: {:?}, tokens: {} prompt + {} completion)",
        response.finish_reason, response.usage.prompt_tokens, response.usage.completion_tokens
    );

    // ── 4. Streaming request ─────────────────────────────────────────────
    // `process_streaming_request` yields each output `Item` as it is produced.
    println!("\n--- Streaming request ---");
    let streaming = Request::from_items(vec![Item::user_message(vec![Item::text(
        "Explain the borrow checker in two sentences.",
    )])]);

    print!("Assistant: ");
    let mut stream = session.process_streaming_request(streaming);
    while let Some(item) = stream.next().await {
        if let Some(text) = item?.as_text() {
            print!("{text}");
            io::stdout().flush().ok();
        }
    }
    println!();

    // ── 5. Multi-turn state ──────────────────────────────────────────────
    // The session retains conversation state; each processed request is a turn.
    println!("\nCompleted {} turn(s).", session.turn_count());

    // ── 6. Unload the model ──────────────────────────────────────────────
    // Drop the session first: the core refuses to unload a model that still
    // has a live session.
    drop(session);
    model.unload().await?;
    println!("Done.");

    Ok(())
}
