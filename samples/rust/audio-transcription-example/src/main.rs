// <complete_code>
// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

// <imports>
use std::env;
use std::io::{self, Write};
use std::path::PathBuf;

use foundry_local_sdk::{FoundryLocalConfig, FoundryLocalManager};
use tokio_stream::StreamExt;
// </imports>

const ALIAS: &str = "whisper-tiny";

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Audio Transcription Example");
    println!("===========================\n");

    // Accept an optional audio file path as a CLI argument, defaulting to
    // ../../assets/audio/Recording.mp3 resolved from CARGO_MANIFEST_DIR.
    let audio_path = env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                .join("../../assets/audio/Recording.mp3")
        });

    // ── 1. Initialise the manager ────────────────────────────────────────
    // <init>
    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("foundry_local_samples"))?;
    // </init>

    // Download and register all execution providers.
    manager
        .download_and_register_eps_with_progress(None, {
            let mut current_ep = String::new();
            move |ep_name: &str, percent: f64| {
                if ep_name != current_ep {
                    if !current_ep.is_empty() {
                        println!();
                    }
                    current_ep = ep_name.to_string();
                }
                print!("\r  {:<30}  {:5.1}%", ep_name, percent);
                io::stdout().flush().ok();
            }
        })
        .await?;
    println!();

    // ── 2. Pick the whispermodel and ensure it is downloaded ────────────
    // <model_setup>
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
    // </model_setup>

    // <transcription>
    // ── 3. Create an audio client────────────────────────────────────────
    let audio_client = model.create_audio_client();

    // ── 4. Non-streaming transcription ───────────────────────────────────
    println!("--- Non-streaming transcription ---");
    let result = audio_client.transcribe(&audio_path).await?;
    println!("Transcription: {}", result.text);

    // ── 5. Streaming transcription ───────────────────────────────────────
    println!("--- Streaming transcription ---");
    print!("Transcription: ");
    let mut stream = audio_client.transcribe_streaming(&audio_path).await?;
    while let Some(chunk) = stream.next().await {
        let chunk = chunk?;
        print!("{}", chunk.text);
        io::stdout().flush().ok();
    }
    println!("\n");
    // </transcription>

    // ── 6. Unload the model──────────────────────────────────────────────
    // <cleanup>
    println!("Unloading model...");
    model.unload().await?;
    println!("Done.");
    // </cleanup>

    Ok(())
}
// </complete_code>
