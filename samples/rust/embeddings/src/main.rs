use std::io::{self, Write};

use foundry_local_sdk::{EmbeddingsSession, FoundryLocalConfig, FoundryLocalManager};

const MODEL_ALIAS: &str = "qwen3-embedding-0.6b";

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("foundry_local_samples"))?;
    manager.download_and_register_eps(None).await?;
    let model = manager.catalog().get_model(MODEL_ALIAS).await?;

    if !model.is_cached().await? {
        model
            .download(Some(|progress| {
                print!("\rDownloading model: {progress:.1}%");
                io::stdout().flush().ok();
            }))
            .await?;
        println!();
    }

    model.load().await?;
    let session = EmbeddingsSession::new(&model).await?;

    let embeddings = session
        .embed_batch(vec![
            "A kitten is sleeping on a rug.".to_string(),
            "A cat rests on a mat.".to_string(),
            "Saturn has a prominent ring system.".to_string(),
        ])
        .await?;

    // EmbeddingsSession returns L2-normalized vectors, so their dot product is cosine similarity.
    let cosine_similarity =
        |left: &[f32], right: &[f32]| left.iter().zip(right).map(|(a, b)| a * b).sum::<f32>();
    println!("Model: {}", model.id());
    println!("Dimensions: {}", embeddings[0].len());
    println!(
        "Similarity (cat, cat): {}",
        cosine_similarity(&embeddings[0], &embeddings[1])
    );
    println!(
        "Similarity (cat, Saturn): {}",
        cosine_similarity(&embeddings[0], &embeddings[2])
    );

    drop(session);
    model.unload().await?;
    manager.shutdown()?;
    Ok(())
}
