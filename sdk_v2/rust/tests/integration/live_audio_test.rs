use super::common;
use std::sync::Arc;
use tokio_stream::StreamExt;

/// Generate synthetic PCM audio (440Hz sine wave, 16kHz, 16-bit mono).
fn generate_sine_wave_pcm(sample_rate: i32, duration_seconds: i32, frequency: f64) -> Vec<u8> {
    let total_samples = (sample_rate * duration_seconds) as usize;
    let mut pcm_bytes = vec![0u8; total_samples * 2]; // 16-bit = 2 bytes per sample

    for i in 0..total_samples {
        let t = i as f64 / sample_rate as f64;
        let sample =
            (i16::MAX as f64 * 0.5 * (2.0 * std::f64::consts::PI * frequency * t).sin()) as i16;
        pcm_bytes[i * 2] = (sample & 0xFF) as u8;
        pcm_bytes[i * 2 + 1] = ((sample >> 8) & 0xFF) as u8;
    }

    pcm_bytes
}

// --- E2E streaming test with synthetic PCM audio ---

#[tokio::test]
async fn live_streaming_e2e_with_synthetic_pcm_returns_valid_response() {
    let manager = common::get_test_manager();
    let catalog = manager.catalog();

    let cached_models = catalog
        .get_cached_models()
        .await
        .expect("get_cached_models failed");
    let mut matching_model = None;
    for model in cached_models {
        let info = model.info().expect("model.info() failed");
        if info.task.as_deref() == Some("automatic-speech-recognition")
            && (info.id.to_lowercase().contains("nemotron")
                || info.name.to_lowercase().contains("nemotron")
                || info.alias.to_lowercase().contains("nemotron"))
        {
            matching_model = Some(model);
            break;
        }
    }
    let model = match matching_model {
        Some(model) => model,
        None => {
            eprintln!("Skipping E2E test: no cached Nemotron streaming-audio model available");
            return;
        }
    };

    model.load().await.expect("model.load() failed");

    let audio_client = model.create_audio_client();
    let session = audio_client.create_live_transcription_session();

    // Verify default settings
    assert_eq!(session.settings.sample_rate, 16000);
    assert_eq!(session.settings.channels, 1);

    // The model is confirmed available, cached, and loaded above, so a failure
    // to start live transcription is a real regression — fail rather than skip.
    session
        .start(None)
        .await
        .expect("live transcription session should start");

    // Start collecting results in background (must start before pushing audio)
    let mut stream = session.get_stream().await.expect("get_stream failed");

    let results = Arc::new(tokio::sync::Mutex::new(Vec::new()));
    let stream_error: Arc<tokio::sync::Mutex<Option<String>>> =
        Arc::new(tokio::sync::Mutex::new(None));
    let results_clone = Arc::clone(&results);
    let error_clone = Arc::clone(&stream_error);
    let read_task = tokio::spawn(async move {
        while let Some(result) = stream.next().await {
            match result {
                Ok(r) => results_clone.lock().await.push(r),
                Err(e) => {
                    *error_clone.lock().await = Some(format!("{e}"));
                    break;
                }
            }
        }
    });

    // Generate ~2 seconds of synthetic PCM audio (440Hz sine wave)
    let pcm_bytes = generate_sine_wave_pcm(16000, 2, 440.0);

    // Push audio in chunks (100ms each, matching typical mic callback size)
    let chunk_size = 16000 / 10 * 2; // 100ms of 16-bit audio = 3200 bytes
    for offset in (0..pcm_bytes.len()).step_by(chunk_size) {
        let end = std::cmp::min(offset + chunk_size, pcm_bytes.len());
        session
            .append(&pcm_bytes[offset..end], None)
            .await
            .expect("append failed");
    }

    // Stop session to flush remaining audio and complete the stream
    session.stop(None).await.expect("stop failed");
    read_task.await.expect("read task failed");

    // Verify no stream errors occurred
    assert!(
        stream_error.lock().await.is_none(),
        "Stream produced an error: {:?}",
        stream_error.lock().await
    );

    // Verify response attributes — synthetic audio may or may not produce text,
    // but the response objects should be properly structured (C#-compatible envelope)
    let results = results.lock().await;
    for result in results.iter() {
        assert!(!result.content.is_empty(), "content must not be empty");
        assert_eq!(result.content[0].text, result.content[0].transcript);
    }

    model.unload().await.expect("model.unload() failed");
}
