//! Integration tests for the low-level inference API: `Session` / `ChatSession`
//! / `EmbeddingsSession` with `Request` / `Response` / `Item`.

use std::sync::Arc;

use foundry_local_sdk::{
    ChatSession, EmbeddingsSession, Item, MessageRole, Model, Request, RequestOptions,
    SearchOptions,
};
use tokio_stream::StreamExt;

use super::common;

async fn setup_chat_session() -> (ChatSession, Arc<Model>) {
    let manager = common::get_test_manager();
    let model = manager
        .catalog()
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed");
    model.load().await.expect("model.load() failed");
    let session = ChatSession::new(&model)
        .await
        .expect("ChatSession::new failed");
    (session, model)
}

async fn setup_embeddings_session() -> (EmbeddingsSession, Arc<Model>) {
    let manager = common::get_test_manager();
    let model = manager
        .catalog()
        .get_model(common::EMBEDDING_MODEL_ALIAS)
        .await
        .expect("embedding model should exist in catalog");
    model.load().await.expect("model.load() failed");
    let session = EmbeddingsSession::new(&model)
        .await
        .expect("EmbeddingsSession::new failed");
    (session, model)
}

/// Deterministic options: greedy decoding with a bounded output length.
fn deterministic_options() -> RequestOptions {
    RequestOptions {
        search: SearchOptions {
            temperature: Some(0.0),
            max_output_tokens: Some(500),
            ..Default::default()
        },
        ..Default::default()
    }
}

#[tokio::test]
async fn should_process_request_and_return_response() {
    let (session, model) = setup_chat_session().await;

    let request = Request::from_items(vec![
        Item::message(
            MessageRole::System,
            vec![Item::text(
                "You are a helpful math assistant. Respond with just the answer.",
            )],
        ),
        Item::user_message(vec![Item::text("What is 7*6?")]),
    ])
    .with_options(deterministic_options());

    let response = session
        .process_request(request)
        .await
        .expect("process_request failed");

    let text = response.text();
    println!("Response: {text}");
    assert!(
        text.contains("42"),
        "Expected response to contain '42', got: {text}"
    );
    assert!(
        !response.items.is_empty(),
        "response should contain at least one item"
    );

    drop(session); // release the session so the model can unload
    model.unload().await.expect("unload should succeed");
}

#[tokio::test]
async fn should_stream_items() {
    let (session, model) = setup_chat_session().await;

    let request = Request::from_items(vec![Item::user_message(vec![Item::text("What is 7*6?")])])
        .with_options(deterministic_options());

    let mut stream = session.process_streaming_request(request);
    let mut collected = String::new();
    while let Some(item) = stream.next().await {
        let item = item.expect("stream item failed");
        if let Some(text) = item.as_text() {
            collected.push_str(text);
        }
    }

    println!("Streamed: {collected}");
    assert!(
        collected.contains("42"),
        "Expected streamed text to contain '42', got: {collected}"
    );

    drop(session); // release the session so the model can unload
    model.unload().await.expect("unload should succeed");
}

#[tokio::test]
async fn should_track_turn_count() {
    let (session, model) = setup_chat_session().await;
    assert_eq!(session.turn_count(), 0, "new session should have 0 turns");

    let request = Request::from_items(vec![Item::user_message(vec![Item::text("Say hi.")])])
        .with_options(deterministic_options());
    session
        .process_request(request)
        .await
        .expect("process_request failed");

    assert!(
        session.turn_count() >= 1,
        "turn count should advance after a processed request, got {}",
        session.turn_count()
    );

    drop(session); // release the session so the model can unload
    model.unload().await.expect("unload should succeed");
}

#[tokio::test]
async fn should_embed_text() {
    let (session, model) = setup_embeddings_session().await;

    let embedding = session
        .embed("The quick brown fox jumps over the lazy dog")
        .await
        .expect("embed failed");

    assert_eq!(embedding.len(), 1024, "unexpected embedding dimension");
    let norm: f32 = embedding.iter().map(|v| v * v).sum::<f32>().sqrt();
    assert!(norm > 0.0, "embedding should be a non-zero vector");

    drop(session); // release the session so the model can unload
    model.unload().await.expect("unload should succeed");
}

#[tokio::test]
async fn should_embed_batch() {
    let (session, model) = setup_embeddings_session().await;

    let inputs = vec![
        "The quick brown fox jumps over the lazy dog".to_string(),
        "Machine learning is a subset of artificial intelligence".to_string(),
        "The capital of France is Paris".to_string(),
    ];
    let vectors = session
        .embed_batch(inputs.clone())
        .await
        .expect("embed_batch failed");

    assert_eq!(vectors.len(), inputs.len(), "one vector per input");
    for vector in &vectors {
        assert_eq!(vector.len(), 1024, "unexpected embedding dimension");
    }

    drop(session); // release the session so the model can unload
    model.unload().await.expect("unload should succeed");
}
