// <complete_code>
// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

//! Foundry Local Web Server vision example (Responses API).
//!
//! Mirrors `samples/python/web-server-responses-vision`. Starts the local
//! Foundry web service, posts a multimodal request to `/v1/responses` with a
//! base64-encoded image, and streams the SSE response, printing each
//! `response.output_text.delta` event.

// <imports>
use std::io::{self, Write};
use std::path::{Path, PathBuf};

use base64::Engine;
use futures_util::StreamExt;
use serde_json::{json, Value};

use foundry_local_sdk::{FoundryLocalConfig, FoundryLocalManager};
// </imports>

const DEFAULT_MODEL_ALIAS: &str = "qwen3.5-0.8b";
const DEFAULT_MAX_OUTPUT_TOKENS: u64 = 8192;

fn print_usage() {
    eprintln!("Usage: cargo run -p foundry-local-webserver-responses-vision -- <model_alias_or_id> [image_path]");
    eprintln!("         cargo run -p foundry-local-webserver-responses-vision -- --list-models");
    eprintln!("  Example: ... -- {DEFAULT_MODEL_ALIAS}");
    eprintln!("  Example: ... -- Qwen2.5-VL-7B-Instruct-generic-cpu");
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() {
        print_usage();
        std::process::exit(1);
    }

    let list_models = matches!(args[0].as_str(), "--list-models" | "-l");

    // <init>
    println!("Initializing Foundry Local SDK...");
    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("foundry_local_samples"))?;
    println!("✓ SDK initialized");

    println!("\nDownloading execution providers:");
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
    // </init>

    if list_models {
        let all_models = manager.catalog().get_models().await?;
        let mut vision_models: Vec<_> = all_models
            .into_iter()
            .filter(|m| {
                m.info()
                    .task
                    .as_deref()
                    .map(|t| t.to_lowercase().contains("vision"))
                    .unwrap_or(false)
            })
            .collect();
        vision_models.sort_by(|a, b| a.alias().cmp(b.alias()));

        if vision_models.is_empty() {
            println!("\nNo vision models found in catalog.");
            return Ok(());
        }

        let total_variants: usize = vision_models.iter().map(|m| m.variants().len()).sum();
        println!(
            "\nVision models in catalog ({} aliases, {} variants):",
            vision_models.len(),
            total_variants
        );
        println!(
            "  {:<32}  {:<20}  {:<20}  {:<24}  CAPABILITIES",
            "ALIAS", "INPUT MODALITIES", "OUTPUT MODALITIES", "TASK"
        );
        for m in &vision_models {
            let info = m.info();
            println!(
                "  {:<32}  {:<20}  {:<20}  {:<24}  {}",
                m.alias(),
                info.input_modalities.as_deref().unwrap_or(""),
                info.output_modalities.as_deref().unwrap_or(""),
                info.task.as_deref().unwrap_or(""),
                info.capabilities.as_deref().unwrap_or(""),
            );

            let mut variants = m.variants();
            if variants.is_empty() {
                continue;
            }
            variants.sort_by(|a, b| {
                let ad = a
                    .info()
                    .runtime
                    .as_ref()
                    .map(|r| format!("{:?}", r.device_type))
                    .unwrap_or_default();
                let bd = b
                    .info()
                    .runtime
                    .as_ref()
                    .map(|r| format!("{:?}", r.device_type))
                    .unwrap_or_default();
                ad.cmp(&bd)
                    .then_with(|| {
                        let ae = a
                            .info()
                            .runtime
                            .as_ref()
                            .map(|r| r.execution_provider.clone())
                            .unwrap_or_default();
                        let be = b
                            .info()
                            .runtime
                            .as_ref()
                            .map(|r| r.execution_provider.clone())
                            .unwrap_or_default();
                        ae.cmp(&be)
                    })
                    .then_with(|| a.id().cmp(b.id()))
            });

            println!(
                "      {:<54}  {:<6}  {:<32}  {:>10}  CACHED",
                "VARIANT ID", "DEVICE", "EXECUTION PROVIDER", "SIZE (MB)"
            );
            for v in &variants {
                let info = v.info();
                let device = info
                    .runtime
                    .as_ref()
                    .map(|r| format!("{:?}", r.device_type))
                    .unwrap_or_default();
                let ep = info
                    .runtime
                    .as_ref()
                    .map(|r| r.execution_provider.as_str())
                    .unwrap_or("");
                let size = match info.file_size_mb {
                    Some(s) => format!("{:>10}", s),
                    None => " ".repeat(10),
                };
                let cached = if v.is_cached().await.unwrap_or(false) { "yes" } else { "no" };
                println!(
                    "      {:<54}  {:<6}  {:<32}  {}  {}",
                    v.id(),
                    device,
                    ep,
                    size,
                    cached
                );
            }
        }
        return Ok(());
    }

    let model_identifier = args[0].clone();
    let default_image = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("test_image.jpg");
    let image_path = if args.len() > 1 {
        PathBuf::from(&args[1])
    } else {
        default_image
    };

    // <model_setup>
    let model = match manager.catalog().get_model(&model_identifier).await {
        Ok(m) => m,
        Err(_) => match manager.catalog().get_model_variant(&model_identifier).await {
            Ok(m) => m,
            Err(_) => {
                let all = manager.catalog().get_models().await?;
                let aliases: Vec<String> = all.iter().map(|m| m.alias().to_string()).collect();
                eprintln!(
                    "\nModel '{}' not found in catalog (tried alias and variant id).",
                    model_identifier
                );
                eprintln!("Available aliases: {:?}", aliases);
                eprintln!("Run with --list-models to see variant ids.");
                std::process::exit(1);
            }
        },
    };

    if !model.is_cached().await? {
        print!("\nDownloading model {model_identifier}...");
        model
            .download(Some(|progress: f64| {
                print!("\rDownloading model: {progress:.2}%");
                io::stdout().flush().ok();
            }))
            .await?;
        println!();
    }

    print!("Loading model {model_identifier}...");
    model.load().await?;
    println!("done.");
    // </model_setup>

    // <server_setup>
    print!("\nStarting web service...");
    manager.start_web_service().await?;
    println!("done.");

    let urls = manager.urls()?;
    let endpoint = urls
        .first()
        .expect("Web service did not return an endpoint");
    let base_url = format!("{}/v1", endpoint.trim_end_matches('/'));
    println!("Web service listening on: {base_url}");
    // </server_setup>

    // <inference>
    println!("\nPreparing image: {}", image_path.display());
    let (image_b64, media_type) = encode_image(&image_path)?;

    // The Foundry Local Responses API accepts an array of message items with input_text /
    // input_image content parts. The input_image part uses Foundry-specific `image_data` and
    // `media_type` fields (in place of OpenAI's `image_url`).
    let vision_input = json!([
        {
            "type": "message",
            "role": "user",
            "content": [
                { "type": "input_text", "text": "Describe this image." },
                { "type": "input_image", "image_data": image_b64, "media_type": media_type }
            ]
        }
    ]);

    let body = json!({
        "model": model.id(),
        "input": vision_input,
        "max_output_tokens": DEFAULT_MAX_OUTPUT_TOKENS,
        "stream": true,
    });

    println!("\nStreaming vision response...");
    // No request timeout: streamed vision responses can take a while to complete.
    // (reqwest treats a zero Duration as "expire immediately" rather than "no timeout",
    // so the timeout is simply left unset here.)
    let client = reqwest::Client::builder().build()?;
    let response = client
        .post(format!("{base_url}/responses"))
        .bearer_auth("notneeded")
        .header("Accept", "text/event-stream")
        .json(&body)
        .send()
        .await?
        .error_for_status()?;

    print!("[ASSISTANT]: ");
    io::stdout().flush().ok();

    let mut stream = response.bytes_stream();
    let mut buf = String::new();
    'outer: while let Some(chunk) = stream.next().await {
        let chunk = chunk?;
        buf.push_str(&String::from_utf8_lossy(&chunk));
        while let Some(nl) = buf.find('\n') {
            let line = buf[..nl].trim_end().to_string();
            buf.drain(..=nl);
            let Some(data) = line.strip_prefix("data: ") else {
                continue;
            };
            if data == "[DONE]" {
                break 'outer;
            }
            if let Ok(event) = serde_json::from_str::<Value>(data) {
                if event.get("type").and_then(|t| t.as_str())
                    == Some("response.output_text.delta")
                {
                    if let Some(delta) = event.get("delta").and_then(|d| d.as_str()) {
                        print!("{delta}");
                        io::stdout().flush().ok();
                    }
                }
            }
        }
    }
    println!();
    // </inference>

    println!("\nStopping web service...");
    manager.stop_web_service().await?;
    println!("Unloading model...");
    model.unload().await?;
    println!("✓ Done.");
    Ok(())
}

fn encode_image(path: &Path) -> Result<(String, &'static str), Box<dyn std::error::Error>> {
    let media_type = match path
        .extension()
        .and_then(|e| e.to_str())
        .map(|s| s.to_ascii_lowercase())
        .as_deref()
    {
        Some("jpg") | Some("jpeg") => "image/jpeg",
        Some("png") => "image/png",
        Some("gif") => "image/gif",
        Some("bmp") => "image/bmp",
        Some("webp") => "image/webp",
        _ => "image/jpeg",
    };
    let bytes = std::fs::read(path)?;
    let b64 = base64::engine::general_purpose::STANDARD.encode(&bytes);
    Ok((b64, media_type))
}
// </complete_code>
