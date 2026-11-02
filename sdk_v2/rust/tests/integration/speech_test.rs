//! Integration tests for [`SpeechSynthesisSession`].
//!
//! Uses the checked-in deterministic `tiny-vibevoice` package (see
//! `sdk_v2/cpp/test/testdata/tiny-vibevoice/generate-model.py`), so no model download is needed.
//! The tiny graphs emit four samples per speech frame and end each chunk at a known frame count,
//! which makes chunking and voice selection observable.

use std::path::PathBuf;

use foundry_local_sdk::{CatalogType, SpeechOptions, SpeechSynthesisSession, TEXT_TO_SPEECH_TASK};
use tokio_stream::StreamExt;

use super::common;

fn tiny_vibevoice_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("cpp")
        .join("test")
        .join("testdata")
        .join("tiny-vibevoice")
        .canonicalize()
        .expect("tiny-vibevoice test package")
}

async fn register_tiny_vibevoice(model_id: &str) -> std::sync::Arc<foundry_local_sdk::Model> {
    let manager = common::get_test_manager();
    let catalog = manager
        .get_catalog(CatalogType::Local)
        .expect("local catalog");
    let _ = catalog.unregister_model(model_id).await;
    let mut metadata = catalog.create_model_info().expect("model info");
    metadata
        .set_string_property("task", TEXT_TO_SPEECH_TASK)
        .expect("task");
    let model = catalog
        .register_model(&tiny_vibevoice_path().to_string_lossy(), model_id, metadata)
        .await
        .expect("register tiny-vibevoice");
    model.load().await.expect("load tiny-vibevoice");
    model
}

#[tokio::test]
async fn should_list_voices_from_genai_config() {
    let model = register_tiny_vibevoice("tiny-vibevoice-voices:1").await;
    let voices = model.speech_voices().await.expect("voices");
    let names: Vec<_> = voices.iter().map(|v| v.name.as_str()).collect();
    assert_eq!(names, ["Alpha", "Beta"]);
    assert_eq!(voices[1].language, "en-in");
    assert_eq!(voices[1].gender.as_deref(), Some("male"));
}

#[tokio::test]
async fn should_synthesize_text_to_audio() {
    let model = register_tiny_vibevoice("tiny-vibevoice-synthesize:1").await;
    let session = SpeechSynthesisSession::new(&model).await.expect("session");

    let audio = session
        .synthesize("Hello world.", SpeechOptions::new())
        .await
        .expect("audio");
    assert_eq!(audio.sample_rate, 24000);
    // 13 tokens with voice Alpha end at 25 frames of four samples.
    assert_eq!(audio.samples().len(), 100);
    assert_eq!(&audio.to_wav()[0..4], b"RIFF");

    // Voice Beta has a longer prompt, so the tiny EOS fires two frames earlier.
    let beta = session
        .synthesize(
            "Hello world.",
            SpeechOptions::new().voice("Beta").speed(1.0),
        )
        .await
        .expect("beta");
    assert_eq!(beta.samples().len(), 92);

    // VibeVoice has no speaking-rate control.
    assert!(session
        .synthesize("Hello world.", SpeechOptions::new().speed(2.0))
        .await
        .is_err());
}

#[tokio::test]
async fn should_stream_one_chunk_per_sentence() {
    let model = register_tiny_vibevoice("tiny-vibevoice-stream:1").await;
    let session = SpeechSynthesisSession::new(&model).await.expect("session");

    let mut stream = session.synthesize_streaming(
        "The first sentence is long enough. Hi. The second one is long too!",
        SpeechOptions::new().voice("Alpha"),
    );
    let mut chunks = Vec::new();
    while let Some(chunk) = stream.next().await {
        chunks.push(chunk.expect("chunk"));
    }
    let complete = stream.complete_audio().await.expect("complete audio");

    assert_eq!(chunks.len(), 2);
    let streamed_samples: usize = chunks.iter().map(|c| c.samples().len()).sum();
    assert_eq!(complete.samples().len(), streamed_samples);
}
