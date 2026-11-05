//! Text to speech with a local VibeVoice-Realtime package.
//!
//! Usage:
//!   cargo run --example text_to_speech -- <package dir> [output.wav] [voice] [text]
//!
//! A catalog model with task `"text-to-speech"` works the same way after `model.download(...)`.

use std::time::Instant;

use foundry_local_sdk::{
    CatalogType, FoundryLocalConfig, FoundryLocalManager, SpeechOptions, SpeechSynthesisSession,
    TEXT_TO_SPEECH_TASK,
};
use tokio_stream::StreamExt;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(1);
    let package_dir = args
        .next()
        .ok_or("usage: text_to_speech <package dir> [output.wav] [voice] [text]")?;
    let output = args.next().unwrap_or_else(|| "speech.wav".into());
    let voice = args.next().filter(|v| !v.is_empty());
    let text = args.next().unwrap_or_else(|| {
        "Hello! This speech runs locally with Foundry Local. Each sentence streams as soon as it is ready."
            .into()
    });

    // Register the package in the local catalog (bring your own model), then list its voices and load it.
    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("text_to_speech_example"))?;
    let catalog = manager.get_catalog(CatalogType::Local)?;
    let model_id = "vibevoice-realtime-0.5b-example:1";
    let _ = catalog.unregister_model(model_id).await;
    let mut metadata = catalog.create_model_info()?;
    metadata.set_string_property("task", TEXT_TO_SPEECH_TASK)?;
    let package_path = std::fs::canonicalize(&package_dir)?;
    let model = catalog
        .register_model(&package_path.to_string_lossy(), model_id, metadata)
        .await?;
    let voices: Vec<_> = model
        .speech_voices()
        .await?
        .into_iter()
        .map(|v| v.name)
        .collect();
    println!("voices: {}", voices.join(", "));
    let start = Instant::now();
    model.load().await?;
    println!("loaded in {:.2} s", start.elapsed().as_secs_f32());

    // Stream one chunk per sentence; a player could start with the first chunk.
    let session = SpeechSynthesisSession::new(&model).await?;
    let options = voice.map_or_else(SpeechOptions::new, |v| SpeechOptions::new().voice(v));
    let start = Instant::now();
    let mut stream = session.synthesize_streaming(text, options);
    while let Some(chunk) = stream.next().await {
        let chunk = chunk?;
        println!(
            "chunk: {:.2} s of audio after {:.2} s",
            chunk.samples().len() as f32 / chunk.sample_rate as f32,
            start.elapsed().as_secs_f32()
        );
    }

    let audio = stream.complete_audio().await?;
    std::fs::write(&output, audio.to_wav())?;
    println!("wrote {output}");
    drop(session);
    model.unload().await?;
    Ok(())
}
