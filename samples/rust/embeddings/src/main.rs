// <complete_code>
// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

use std::io::{self, Write};

use foundry_local_sdk::{EmbeddingsSession, FoundryLocalConfig, FoundryLocalManager};

const ALIAS: &str = "qwen3-embedding-0.6b";

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Native Embeddings");
    println!("=================\n");

    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("foundry_local_samples"))?;

    let model = manager.catalog().get_model(ALIAS).await?;
    println!("Model: {} (id: {})", model.alias(), model.id());

    if !model.is_cached().await? {
        println!("Downloading model...");
        model
            .download(Some(|progress: f64| {
                print!("\r  {progress:.1}%");
                io::stdout().flush().ok();
            }))
            .await?;
        println!();
    }

    println!("Loading model...");
    model.load().await?;
    println!("✓ Model loaded\n");

    let session = EmbeddingsSession::new(&model).await?;

    println!("--- Single Embedding ---");
    let embedding = session
        .embed("The quick brown fox jumps over the lazy dog")
        .await?;
    println!("Dimensions: {}", embedding.len());
    println!("First 5 values: {:?}", &embedding[..5.min(embedding.len())]);

    println!("\n--- Batch Embeddings ---");
    let batch = session
        .embed_batch(vec![
            "Machine learning is a subset of artificial intelligence".to_string(),
            "The capital of France is Paris".to_string(),
            "Rust is a systems programming language".to_string(),
        ])
        .await?;
    println!("Number of embeddings: {}", batch.len());
    for (i, vector) in batch.iter().enumerate() {
        println!("  [{i}] Dimensions: {}", vector.len());
    }

    drop(session);
    println!("\nUnloading model...");
    model.unload().await?;
    println!("Done.");

    Ok(())
}
// </complete_code>
