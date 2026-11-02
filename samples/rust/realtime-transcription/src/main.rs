use std::io::{self, Write};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use foundry_local_sdk::{
    AudioSession, FoundryLocalConfig, FoundryLocalManager, Item, Request, RequestOptions,
    SpeechSegmentKind,
};
use tokio_stream::StreamExt;

const MODEL_ALIAS: &str = "nemotron-speech-streaming-en-0.6b";

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let running = Arc::new(AtomicBool::new(true));
    let signal = Arc::clone(&running);
    ctrlc::set_handler(move || signal.store(false, Ordering::SeqCst))?;

    let manager = FoundryLocalManager::create(FoundryLocalConfig::new("foundry_local_samples"))?;
    manager.download_and_register_eps(None).await?;
    let model = manager.catalog().get_model(MODEL_ALIAS).await?;

    if !model.is_cached().await? {
        model.download(None::<fn(f64)>).await?;
    }
    model.load().await?;

    let session = AudioSession::new(&model).await?;
    let queue = session.create_input_queue()?;
    let request = Request::from_items(vec![Item::audio_data(
        Vec::<u8>::new(),
        Some("pcm"),
        16000,
        1,
    )])
    .with_input_queue(queue.clone())
    .with_options(RequestOptions {
        additional_options: vec![("language".to_string(), "en".to_string())],
        ..Default::default()
    });

    let device = cpal::default_host()
        .default_input_device()
        .ok_or("No default input device is available")?;
    let supported = device.default_input_config()?;
    let sample_rate = supported.sample_rate().0;
    let channels = supported.channels();
    let config: cpal::StreamConfig = supported.clone().into();
    let (audio_tx, mut audio_rx) = tokio::sync::mpsc::channel::<Vec<f32>>(32);
    let stream = match supported.sample_format() {
        cpal::SampleFormat::F32 => {
            let audio_tx = audio_tx.clone();
            device.build_input_stream(
                &config,
                move |data: &[f32], _| {
                    let _ = audio_tx.try_send(data.to_vec());
                },
                |error| eprintln!("Microphone error: {error}"),
                None,
            )?
        }
        cpal::SampleFormat::I16 => {
            let audio_tx = audio_tx.clone();
            device.build_input_stream(
                &config,
                move |data: &[i16], _| {
                    let samples = data
                        .iter()
                        .map(|sample| *sample as f32 / i16::MAX as f32)
                        .collect();
                    let _ = audio_tx.try_send(samples);
                },
                |error| eprintln!("Microphone error: {error}"),
                None,
            )?
        }
        cpal::SampleFormat::U16 => device.build_input_stream(
            &config,
            move |data: &[u16], _| {
                let samples = data
                    .iter()
                    .map(|sample| (*sample as f32 / u16::MAX as f32) * 2.0 - 1.0)
                    .collect();
                let _ = audio_tx.try_send(samples);
            },
            |error| eprintln!("Microphone error: {error}"),
            None,
        )?,
        format => return Err(format!("Unsupported microphone sample format: {format:?}").into()),
    };

    stream.play()?;
    let mut results = session.process_streaming_request(request);
    let result_task = tokio::spawn(async move {
        while let Some(item) = results.next().await {
            if let Item::SpeechSegment(segment) = item? {
                match segment.kind {
                    SpeechSegmentKind::None => print!("{}", segment.text),
                    SpeechSegmentKind::Partial => print!("\r{}", segment.text),
                    SpeechSegmentKind::Final => println!("\n[final] {}", segment.text),
                }
                io::stdout().flush().ok();
            }
        }
        let response = results.response().await?;
        for item in &response.items {
            if let Some(result) = item.as_speech_result() {
                println!("\nTranscript: {}", result.text);
            }
        }
        Ok::<_, foundry_local_sdk::FoundryLocalError>(())
    });

    println!("Listening. Press Ctrl+C to stop.");
    while running.load(Ordering::SeqCst) && !result_task.is_finished() {
        if let Ok(Some(samples)) =
            tokio::time::timeout(Duration::from_millis(100), audio_rx.recv()).await
        {
            let pcm = convert_to_mono_16khz_pcm(&samples, channels, sample_rate);
            queue.push(&Item::bytes(pcm))?;
        }
    }

    drop(stream);
    queue.mark_finished();
    result_task.await??;
    drop(session);
    model.unload().await?;
    manager.shutdown()?;
    Ok(())
}

fn convert_to_mono_16khz_pcm(samples: &[f32], channels: u16, sample_rate: u32) -> Vec<u8> {
    let mono: Vec<f32> = samples
        .chunks(channels as usize)
        .map(|frame| frame.iter().sum::<f32>() / channels as f32)
        .collect();
    let ratio = sample_rate as f64 / 16_000.0;
    let output_len = (mono.len() as f64 / ratio).ceil() as usize;
    let mut pcm = Vec::with_capacity(output_len * 2);

    for output_index in 0..output_len {
        let source_position = output_index as f64 * ratio;
        let index = source_position as usize;
        let fraction = (source_position - index as f64) as f32;
        let first = mono[index.min(mono.len().saturating_sub(1))];
        let second = mono[(index + 1).min(mono.len().saturating_sub(1))];
        let sample = (first + (second - first) * fraction).clamp(-1.0, 1.0);
        pcm.extend_from_slice(&((sample * i16::MAX as f32) as i16).to_le_bytes());
    }

    pcm
}
