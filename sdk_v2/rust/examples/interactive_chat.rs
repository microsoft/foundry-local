//! Interactive chat example — a simple terminal chatbot powered by Foundry Local.
//!
//! Run with: `cargo run --example interactive_chat`

use std::io::{self, Write};

use foundry_local_sdk::{
    ChatSession, FoundryLocalConfig, FoundryLocalManager, Item, Request, RequestOptions,
    SearchOptions,
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
    // `ChatSession` retains conversation history internally, so each turn only
    // needs to send the new user message — no manual message bookkeeping.
    let session = ChatSession::new(&model).await?;
    session
        .set_options(RequestOptions {
            search: SearchOptions {
                temperature: Some(0.7),
                max_output_tokens: Some(512),
                ..Default::default()
            },
            ..Default::default()
        })
        .await?;

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

        // Seed the system prompt on the first turn only; the session keeps it.
        let mut items = Vec::new();
        if session.turn_count() == 0 {
            items.push(Item::system_message(vec![Item::text(
                "You are a helpful, concise assistant.",
            )]));
        }
        items.push(Item::user_message(vec![Item::text(input)]));

        // Stream the response token by token. The session records the turn, so
        // there is no need to append the reply to a local history buffer.
        print!("Assistant: ");
        io::stdout().flush()?;

        let mut stream = session.process_streaming_request(Request::from_items(items));
        while let Some(item) = stream.next().await {
            if let Some(text) = item?.as_text() {
                print!("{text}");
                io::stdout().flush().ok();
            }
        }
        println!("\n");
    }

    // ── Cleanup ──────────────────────────────────────────────────────────
    // Drop the session first: the core refuses to unload a model that still
    // has a live session.
    drop(session);
    println!("\nUnloading model…");
    model.unload().await?;
    println!("Goodbye!");

    Ok(())
}
