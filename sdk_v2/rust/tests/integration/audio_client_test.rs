use super::common;
use foundry_local_sdk::openai::AudioClient;
use std::sync::Arc;
use tokio_stream::StreamExt;

async fn setup_audio_client() -> (AudioClient, Arc<foundry_local_sdk::Model>) {
    let manager = common::get_test_manager();
    let catalog = manager.catalog();
    let model = catalog
        .get_model(common::WHISPER_MODEL_ALIAS)
        .await
        .expect("get_model(whisper-tiny) failed");
    model.load().await.expect("model.load() failed");
    (model.create_audio_client(), model)
}

fn audio_file() -> String {
    common::get_audio_file_path().to_string_lossy().into_owned()
}

#[tokio::test]
async fn should_transcribe_audio_without_streaming() {
    let (client, model) = setup_audio_client().await;
    let client = client.language("en").temperature(0.0);
    let response = client
        .transcribe(&audio_file())
        .await
        .expect("transcribe failed");

    common::assert_transcript_semantically_matches(&response.text);

    model.unload().await.expect("model.unload() failed");
}

#[tokio::test]
async fn should_transcribe_audio_without_streaming_with_temperature() {
    let (client, model) = setup_audio_client().await;
    let client = client.language("en").temperature(0.5);

    let response = client
        .transcribe(&audio_file())
        .await
        .expect("transcribe with temperature failed");

    common::assert_transcript_semantically_matches(&response.text);

    model.unload().await.expect("model.unload() failed");
}

#[tokio::test]
async fn should_transcribe_audio_with_streaming() {
    let (client, model) = setup_audio_client().await;
    let client = client.language("en").temperature(0.0);
    let mut full_text = String::new();

    let mut stream = client
        .transcribe_streaming(&audio_file())
        .await
        .expect("transcribe_streaming setup failed");

    while let Some(chunk) = stream.next().await {
        let chunk = chunk.expect("stream chunk error");
        full_text.push_str(&chunk.text);
    }

    println!("Streamed transcription: {full_text}");

    common::assert_transcript_semantically_matches(&full_text);

    model.unload().await.expect("model.unload() failed");
}

#[tokio::test]
async fn should_transcribe_audio_with_streaming_with_temperature() {
    let (client, model) = setup_audio_client().await;
    let client = client.language("en").temperature(0.5);

    let mut full_text = String::new();

    let mut stream = client
        .transcribe_streaming(&audio_file())
        .await
        .expect("transcribe_streaming with temperature setup failed");

    while let Some(chunk) = stream.next().await {
        let chunk = chunk.expect("stream chunk error");
        full_text.push_str(&chunk.text);
    }

    println!("Streamed transcription: {full_text}");

    common::assert_transcript_semantically_matches(&full_text);

    model.unload().await.expect("model.unload() failed");
}

#[tokio::test]
async fn should_throw_when_transcribing_with_empty_audio_file_path() {
    let (client, model) = setup_audio_client().await;
    let result = client.transcribe("").await;
    assert!(result.is_err(), "Expected error for empty audio file path");

    model.unload().await.expect("model.unload() failed");
}

#[tokio::test]
async fn should_throw_when_transcribing_streaming_with_empty_audio_file_path() {
    let (client, model) = setup_audio_client().await;
    let result = client.transcribe_streaming("").await;
    assert!(
        result.is_err(),
        "Expected error for empty audio file path in streaming"
    );

    model.unload().await.expect("model.unload() failed");
}
