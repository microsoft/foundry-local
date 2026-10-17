// <complete_code>
// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

use std::env;
use std::io::{self, Write};

use foundry_local_sdk::{AudioSession, FoundryLocalConfig, FoundryLocalManager, Item, Request};
use tokio_stream::StreamExt;

const ALIAS: &str = "whisper-tiny";

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Audio Transcription Example");
    println!("===========================\n");

    let audio_path = env::args()
        .nth(1)
        .unwrap_or_else(|| "Recording.mp3".to_string());

    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("foundry_local_samples"))?;

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

    let session = AudioSession::new(&model).await?;

    println!("--- Non-streaming transcription ---");
    let result = session
        .transcribe(Item::audio_uri(&audio_path, None::<String>, 0, 0))
        .await?;
    println!("Transcription: {result}");

    println!("--- Streaming transcription ---");
    print!("Transcription: ");
    let mut stream = session.process_streaming_request(Request::from_items(vec![Item::audio_uri(
        &audio_path,
        None::<String>,
        0,
        0,
    )]));
    while let Some(item) = stream.next().await {
        if let foundry_local_sdk::Item::SpeechSegment(segment) = item? {
            if !segment.text.is_empty() {
                print!("{}", segment.text);
                io::stdout().flush().ok();
            }
        }
    }
    let final_response = stream.response().await?;
    if let Some(result) = final_response
        .items
        .iter()
        .find_map(|item| item.as_speech_result())
    {
        println!("\n[aggregate] {}", result.text);
    } else {
        println!();
    }

    drop(session);
    println!("\nUnloading model...");
    model.unload().await?;
    println!("Done.");

    Ok(())
}
// </complete_code>
