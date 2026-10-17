// Live Audio Transcription — Foundry Local Rust SDK Example
//
// Tries CPAL mic capture first; falls back to synthetic PCM if unavailable.
//
// Usage:
//   cargo run            # Live microphone (press Ctrl+C to stop)
//   cargo run -- --synth # Synthetic 440Hz sine wave

use std::env;
use std::io::{self, Write};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use foundry_local_sdk::{
    AudioSession, FoundryLocalConfig, FoundryLocalManager, Item, Request, RequestOptions,
    SpeechSegmentKind,
};
use tokio_stream::StreamExt;

const ALIAS: &str = "nemotron-speech-streaming-en-0.6b";
static RUNNING: AtomicBool = AtomicBool::new(true);

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let use_synth = env::args().any(|a| a == "--synth");

    ctrlc::set_handler(move || {
        RUNNING.store(false, Ordering::SeqCst);
    })?;

    println!("===========================================================");
    println!("   Foundry Local -- Live Audio Transcription Demo (Rust)");
    println!("===========================================================");
    println!();

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

    let session = AudioSession::new(&model).await?;
    let queue = session.create_input_queue()?;
    // Rust's current typed surface does not expose a dedicated "format descriptor"
    // constructor, so we use an empty Audio item to declare the stream format once
    // on the request. The queue itself must carry BYTES items only.
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

    let (mic_stream, _audio_task) = if !use_synth {
        match start_microphone_capture(queue.clone()) {
            Ok(state) => {
                println!("===========================================================");
                println!("  LIVE TRANSCRIPTION ACTIVE");
                println!("  Speak into your microphone.");
                println!("  Press Ctrl+C to stop.");
                println!("===========================================================");
                println!();
                (Some(state.stream), Some(state.forward_task))
            }
            Err(e) => {
                eprintln!("Could not initialize microphone: {e}");
                eprintln!("Falling back to synthetic audio test...\n");
                spawn_synthetic_audio(queue.clone());
                (None, None)
            }
        }
    } else {
        spawn_synthetic_audio(queue.clone());
        (None, None)
    };

    let mut stream = session.process_streaming_request(request);
    while let Some(item) = stream.next().await {
        match item? {
            Item::SpeechSegment(segment) if !segment.text.is_empty() => match segment.kind {
                SpeechSegmentKind::None | SpeechSegmentKind::Partial => {
                    print!("{}", segment.text);
                    io::stdout().flush().ok();
                }
                SpeechSegmentKind::Final => {
                    println!();
                    println!("  [FINAL] {}", segment.text);
                }
            },
            _ => {}
        }

        if !RUNNING.load(Ordering::SeqCst) && mic_stream.is_some() {
            queue.mark_finished();
        }
    }

    let final_response = stream.response().await?;
    if let Some(result) = final_response
        .items
        .iter()
        .find_map(|item| item.as_speech_result())
    {
        println!("\n[aggregate] {}", result.text);
    }

    drop(mic_stream);
    drop(session);
    model.unload().await?;
    println!("✓ Done");
    Ok(())
}

struct MicState {
    stream: cpal::Stream,
    forward_task: tokio::task::JoinHandle<()>,
}

fn start_microphone_capture(
    queue: foundry_local_sdk::ItemQueue,
) -> Result<MicState, Box<dyn std::error::Error>> {
    let host = cpal::default_host();
    let device = host
        .default_input_device()
        .ok_or("No input audio device available")?;
    let default_config = device.default_input_config()?;
    let device_rate = default_config.sample_rate().0;
    let device_channels = default_config.channels();
    let sample_format = default_config.sample_format();

    let mic_config = cpal::StreamConfig {
        channels: device_channels,
        sample_rate: cpal::SampleRate(device_rate),
        buffer_size: cpal::BufferSize::Default,
    };

    let (audio_tx, mut audio_rx) = tokio::sync::mpsc::channel::<Vec<u8>>(100);
    let err_fn = |err| eprintln!("Microphone stream error: {err}");

    let stream = match sample_format {
        cpal::SampleFormat::F32 => {
            let tx = audio_tx.clone();
            device.build_input_stream(
                &mic_config,
                move |data: &[f32], _: &cpal::InputCallbackInfo| {
                    let bytes = convert_audio(data, device_channels, device_rate);
                    if !bytes.is_empty() {
                        let _ = tx.try_send(bytes);
                    }
                },
                err_fn,
                None,
            )?
        }
        cpal::SampleFormat::I16 => {
            let tx = audio_tx.clone();
            device.build_input_stream(
                &mic_config,
                move |data: &[i16], _: &cpal::InputCallbackInfo| {
                    let samples: Vec<f32> =
                        data.iter().map(|&s| s as f32 / i16::MAX as f32).collect();
                    let bytes = convert_audio(&samples, device_channels, device_rate);
                    if !bytes.is_empty() {
                        let _ = tx.try_send(bytes);
                    }
                },
                err_fn,
                None,
            )?
        }
        cpal::SampleFormat::U16 => {
            let tx = audio_tx.clone();
            device.build_input_stream(
                &mic_config,
                move |data: &[u16], _: &cpal::InputCallbackInfo| {
                    let samples: Vec<f32> = data
                        .iter()
                        .map(|&s| (s as f32 / u16::MAX as f32) * 2.0 - 1.0)
                        .collect();
                    let bytes = convert_audio(&samples, device_channels, device_rate);
                    if !bytes.is_empty() {
                        let _ = tx.try_send(bytes);
                    }
                },
                err_fn,
                None,
            )?
        }
        other => {
            return Err(format!("Unsupported input sample format: {other:?}").into());
        }
    };
    drop(audio_tx);
    stream.play()?;

    let running = Arc::new(AtomicBool::new(true));
    let running_forward = Arc::clone(&running);
    let forward_task = tokio::spawn(async move {
        while running_forward.load(Ordering::SeqCst) && RUNNING.load(Ordering::SeqCst) {
            match audio_rx.recv().await {
                Some(bytes) => {
                    let item = Item::bytes(bytes);
                    if queue.push(&item).is_err() {
                        break;
                    }
                }
                None => break,
            }
        }
        queue.mark_finished();
    });

    Ok(MicState {
        stream,
        forward_task,
    })
}

fn spawn_synthetic_audio(queue: foundry_local_sdk::ItemQueue) {
    tokio::spawn(async move {
        println!("Pushing synthetic audio (440Hz sine, 2s)...");
        let pcm_data = generate_sine_wave_pcm(16000, 2, 440.0);
        let chunk_size = 16000 / 10 * 2;
        for offset in (0..pcm_data.len()).step_by(chunk_size) {
            let end = std::cmp::min(offset + chunk_size, pcm_data.len());
            let item = Item::bytes(pcm_data[offset..end].to_vec());
            if queue.push(&item).is_err() {
                break;
            }
            tokio::time::sleep(std::time::Duration::from_millis(100)).await;
        }
        println!("✓ Synthetic audio pushed");
        queue.mark_finished();
    });
}

fn convert_audio(data: &[f32], channels: u16, sample_rate: u32) -> Vec<u8> {
    let mono: Vec<f32> = if channels > 1 {
        data.chunks(channels as usize)
            .map(|frame| frame.iter().sum::<f32>() / channels as f32)
            .collect()
    } else {
        data.to_vec()
    };

    let resampled = if sample_rate != 16000 {
        resample_linear(&mono, sample_rate, 16000)
    } else {
        mono
    };

    let mut pcm16 = Vec::with_capacity(resampled.len() * 2);
    for sample in resampled {
        let s = (sample.clamp(-1.0, 1.0) * i16::MAX as f32) as i16;
        pcm16.extend_from_slice(&s.to_le_bytes());
    }
    pcm16
}

fn resample_linear(input: &[f32], src_rate: u32, dst_rate: u32) -> Vec<f32> {
    if src_rate == dst_rate || input.is_empty() {
        return input.to_vec();
    }

    let ratio = dst_rate as f64 / src_rate as f64;
    let out_len = (input.len() as f64 * ratio).round() as usize;
    let mut out = Vec::with_capacity(out_len);

    for i in 0..out_len {
        let src_pos = i as f64 / ratio;
        let idx = src_pos.floor() as usize;
        let frac = (src_pos - idx as f64) as f32;
        let a = *input.get(idx).unwrap_or(&0.0);
        let b = *input.get(idx + 1).unwrap_or(&a);
        out.push(a + (b - a) * frac);
    }

    out
}

fn generate_sine_wave_pcm(
    sample_rate: usize,
    duration_seconds: usize,
    frequency_hz: f32,
) -> Vec<u8> {
    let total_samples = sample_rate * duration_seconds;
    let mut pcm = Vec::with_capacity(total_samples * 2);
    for i in 0..total_samples {
        let t = i as f32 / sample_rate as f32;
        let sample =
            (i16::MAX as f32 * 0.5 * (2.0 * std::f32::consts::PI * frequency_hz * t).sin()) as i16;
        pcm.extend_from_slice(&sample.to_le_bytes());
    }
    pcm
}
