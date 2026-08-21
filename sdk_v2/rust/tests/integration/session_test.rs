//! Integration tests for the low-level inference API: `Session` / `ChatSession`
//! / `EmbeddingsSession` with `Request` / `Response` / `Item`.

use std::sync::Arc;

use foundry_local_sdk::{
    ChatSession, EmbeddingsSession, FinishReason, FoundryLocalError, Item, MessageRole, Model,
    NativeErrorCode, Request, RequestOptions, SearchOptions,
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
async fn should_reject_wrong_task_at_construction() {
    let manager = common::get_test_manager();
    let model = manager
        .catalog()
        .get_model(common::TEST_MODEL_ALIAS)
        .await
        .expect("get_model failed");

    // The chat model's task is not "embeddings". The typed constructor must
    // reject it up front — before any load — with a Validation error, rather
    // than deferring to a native "model must be loaded" / task failure. This is
    // the construction-time contract shared by every binding.
    match EmbeddingsSession::new(&model).await {
        Ok(_) => panic!("EmbeddingsSession from a chat model should fail"),
        Err(err) => assert!(
            matches!(err, FoundryLocalError::Validation { .. }),
            "expected a Validation error, got: {err:?}"
        ),
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

    let response = stream
        .response()
        .await
        .expect("terminal streaming response failed");
    assert!(
        matches!(
            response.finish_reason,
            FinishReason::Stop | FinishReason::Length
        ),
        "unexpected finish reason: {:?}",
        response.finish_reason
    );
    assert!(
        response.usage.total_tokens > 0,
        "terminal response should report token usage"
    );
    assert_eq!(
        response.usage.total_tokens,
        response.usage.prompt_tokens + response.usage.completion_tokens
    );

    drop(session); // release the session so the model can unload
    model.unload().await.expect("unload should succeed");
}

#[tokio::test]
async fn should_expose_native_error_code_on_failure() {
    let (session, model) = setup_chat_session().await;

    // Undoing more turns than exist is rejected by the native inference layer
    // with a stable INVALID_USAGE code. Verify the native error identity
    // (code + message) survives the FFI boundary rather than being flattened
    // into an opaque string.
    let err = session
        .undo_turns(5)
        .await
        .expect_err("undo_turns beyond the turn count should fail");

    assert_eq!(
        err.native_code(),
        Some(NativeErrorCode::InvalidUsage),
        "expected a native InvalidUsage error, got: {err:?}"
    );
    assert!(
        err.native_message().is_some_and(|m| !m.is_empty()),
        "native error should carry a non-empty message: {err:?}"
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
