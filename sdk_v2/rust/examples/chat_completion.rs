//! Basic chat completion example demonstrating synchronous and streaming
//! usage of the Foundry Local SDK's `Session` API.

use std::io::{self, Write};

use foundry_local_sdk::{
    ChatSession, FoundryLocalConfig, FoundryLocalError, FoundryLocalManager, Item, Request,
    RequestOptions, SearchOptions,
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

    // ── 4. Open a chat session ───────────────────────────────────────────
    // `ChatSession` retains conversation state across turns; options set here
    // persist, so each request only needs to carry the new input.
    let session = ChatSession::new(&model).await?;
    session
        .set_options(RequestOptions {
            search: SearchOptions {
                temperature: Some(0.7),
                max_output_tokens: Some(256),
                ..Default::default()
            },
            ..Default::default()
        })
        .await?;

    // ── 5. Synchronous request ───────────────────────────────────────────
    let request = Request::from_items(vec![
        Item::system_message(vec![Item::text("You are a helpful assistant.")]),
        Item::user_message(vec![Item::text("What is Rust's ownership model?")]),
    ]);

    println!("\n--- Synchronous completion ---");
    let response = session.process_request(request).await?;
    println!("Assistant: {}", response.text());

    // ── 6. Streaming request (continues the same conversation) ───────────
    // The session already holds the system prompt and the previous turn, so we
    // only send the new user message here.
    println!("\n--- Streaming completion ---");
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

    // ── 7. Unload the model──────────────────────────────────────────────
    // Drop the session first: the core refuses to unload a model that still
    // has a live session.
    drop(session);
    println!("\nUnloading model…");
    model.unload().await?;
    println!("Done.");

    Ok(())
}
